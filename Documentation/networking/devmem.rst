.. SPDX-License-Identifier: GPL-2.0

=================
Device Memory TCP
=================


Intro
=====

Device memory TCP (devmem TCP) enables receiving data directly into device
memory (dmabuf). The feature is currently implemented for TCP sockets.


Opportunity
-----------

A large number of data transfers have device memory as the source and/or
destination. Accelerators drastically increased the prevalence of such
transfers.  Some examples include:

- Distributed training, where ML accelerators, such as GPUs on different hosts,
  exchange data.

- Distributed raw block storage applications transfer large amounts of data with
  remote SSDs. Much of this data does not require host processing.

Typically the Device-to-Device data transfers in the network are implemented as
the following low-level operations: Device-to-Host copy, Host-to-Host network
transfer, and Host-to-Device copy.

The flow involving host copies is suboptimal, especially for bulk data transfers,
and can put significant strains on system resources such as host memory
bandwidth and PCIe bandwidth.

Devmem TCP optimizes this use case by implementing socket APIs that enable
the user to receive incoming network packets directly into device memory.

Packet payloads go directly from the NIC to device memory.

Packet headers go to host memory and are processed by the TCP/IP stack
normally. The NIC must support header split to achieve this.

Advantages:

- Alleviate host memory bandwidth pressure, compared to existing
  network-transfer + device-copy semantics.

- Alleviate PCIe bandwidth pressure, by limiting data transfer to the lowest
  level of the PCIe tree, compared to the traditional path which sends data
  through the root complex.


More Info
---------

  slides, video
    https://netdevconf.org/0x17/sessions/talk/device-memory-tcp.html

  patchset
    [PATCH net-next v24 00/13] Device Memory TCP
    https://lore.kernel.org/netdev/20240831004313.3713467-1-almasrymina@google.com/


RX Interface
============


Example
-------

./tools/testing/selftests/drivers/net/hw/ncdevmem:do_server shows an example of
setting up the RX path of this API.


NIC Setup
---------

Header split, flow steering, & RSS are required features for devmem TCP.

Header split is used to split incoming packets into a header buffer in host
memory, and a payload buffer in device memory.

Flow steering & RSS are used to ensure that only flows targeting devmem land on
an RX queue bound to devmem.

Enable header split & flow steering::

	# enable header split
	ethtool -G eth1 tcp-data-split on


	# enable flow steering
	ethtool -K eth1 ntuple on

Configure RSS to steer all traffic away from the target RX queue (queue 15 in
this example)::

	ethtool --set-rxfh-indir eth1 equal 15


The user must bind a dmabuf to any number of RX queues on a given NIC using
the netlink API::

	/* Bind dmabuf to NIC RX queue 15 */
	struct netdev_queue *queues;
	queues = malloc(sizeof(*queues) * 1);

	queues[0]._present.type = 1;
	queues[0]._present.idx = 1;
	queues[0].type = NETDEV_RX_QUEUE_TYPE_RX;
	queues[0].idx = 15;

	*ys = ynl_sock_create(&ynl_netdev_family, &yerr);

	req = netdev_bind_rx_req_alloc();
	netdev_bind_rx_req_set_ifindex(req, 1 /* ifindex */);
	netdev_bind_rx_req_set_dmabuf_fd(req, dmabuf_fd);
	__netdev_bind_rx_req_set_queues(req, queues, n_queue_index);

	rsp = netdev_bind_rx(*ys, req);

	dmabuf_id = rsp->dmabuf_id;


The netlink API returns a dmabuf_id: a unique ID that refers to this dmabuf
that has been bound.

The user can unbind the dmabuf from the netdevice by closing the netlink socket
that established the binding. We do this so that the binding is automatically
unbound even if the userspace process crashes.

Note that any reasonably well-behaved dmabuf from any exporter should work with
devmem TCP, even if the dmabuf is not actually backed by devmem. An example of
this is udmabuf, which wraps user memory (non-devmem) in a dmabuf.


Socket Setup
------------

The socket must be flow steered to the dmabuf bound RX queue::

	ethtool -N eth1 flow-type tcp4 ... queue 15


Receiving data
--------------

The user application must signal to the kernel that it is capable of receiving
devmem data by passing the MSG_SOCK_DEVMEM flag to recvmsg::

	ret = recvmsg(fd, &msg, MSG_SOCK_DEVMEM);

Applications that do not specify the MSG_SOCK_DEVMEM flag will receive an EFAULT
on devmem data.

Devmem data is received directly into the dmabuf bound to the NIC in 'NIC
Setup', and the kernel signals such to the user via the SCM_DEVMEM_* cmsgs::

		for (cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
			if (cm->cmsg_level != SOL_SOCKET ||
				(cm->cmsg_type != SCM_DEVMEM_DMABUF &&
				 cm->cmsg_type != SCM_DEVMEM_LINEAR))
				continue;

			dmabuf_cmsg = (struct dmabuf_cmsg *)CMSG_DATA(cm);

			if (cm->cmsg_type == SCM_DEVMEM_DMABUF) {
				/* Frag landed in dmabuf.
				 *
				 * dmabuf_cmsg->dmabuf_id is the dmabuf the
				 * frag landed on.
				 *
				 * dmabuf_cmsg->frag_offset is the offset into
				 * the dmabuf where the frag starts.
				 *
				 * dmabuf_cmsg->frag_size is the size of the
				 * frag.
				 *
				 * dmabuf_cmsg->frag_token is a token used to
				 * refer to this frag for later freeing.
				 */

				struct dmabuf_token token;
				token.token_start = dmabuf_cmsg->frag_token;
				token.token_count = 1;
				continue;
			}

			if (cm->cmsg_type == SCM_DEVMEM_LINEAR)
				/* Frag landed in linear buffer.
				 *
				 * dmabuf_cmsg->frag_size is the size of the
				 * frag.
				 */
				continue;

		}

Applications may receive 2 cmsgs:

- SCM_DEVMEM_DMABUF: this indicates the fragment landed in the dmabuf indicated
  by dmabuf_id.

- SCM_DEVMEM_LINEAR: this indicates the fragment landed in the linear buffer.
  This typically happens when the NIC is unable to split the packet at the
  header boundary, such that part (or all) of the payload landed in host
  memory.

Applications may receive no SO_DEVMEM_* cmsgs. That indicates non-devmem,
regular TCP data that landed on an RX queue not bound to a dmabuf.


Freeing frags
-------------

Frags received via SCM_DEVMEM_DMABUF are pinned by the kernel while the user
processes the frag. The user must return the frag to the kernel via
SO_DEVMEM_DONTNEED::

	ret = setsockopt(client_fd, SOL_SOCKET, SO_DEVMEM_DONTNEED, &token,
			 sizeof(token));

The user must ensure the tokens are returned to the kernel in a timely manner.
Failure to do so will exhaust the limited dmabuf that is bound to the RX queue
and will lead to packet drops.

The user must pass no more than 128 tokens, with no more than 1024 total frags
among the token->token_count across all the tokens. If the user provides more
than 1024 frags, the kernel will free up to 1024 frags and return early.

The kernel returns the number of actual frags freed. The number of frags freed
can be less than the tokens provided by the user in case of:

(a) an internal kernel leak bug.
(b) the user passed more than 1024 frags.

TX Interface
============


Example
-------

./tools/testing/selftests/drivers/net/hw/ncdevmem:do_client shows an example of
setting up the TX path of this API.


NIC Setup
---------

The user must bind a TX dmabuf to a given NIC using the netlink API::

        struct netdev_bind_tx_req *req = NULL;
        struct netdev_bind_tx_rsp *rsp = NULL;
        struct ynl_error yerr;

        *ys = ynl_sock_create(&ynl_netdev_family, &yerr);

        req = netdev_bind_tx_req_alloc();
        netdev_bind_tx_req_set_ifindex(req, ifindex);
        netdev_bind_tx_req_set_fd(req, dmabuf_fd);

        rsp = netdev_bind_tx(*ys, req);

        tx_dmabuf_id = rsp->id;


The netlink API returns a dmabuf_id: a unique ID that refers to this dmabuf
that has been bound.

The user can unbind the dmabuf from the netdevice by closing the netlink socket
that established the binding. We do this so that the binding is automatically
unbound even if the userspace process crashes.

Note that any reasonably well-behaved dmabuf from any exporter should work with
devmem TCP, even if the dmabuf is not actually backed by devmem. An example of
this is udmabuf, which wraps user memory (non-devmem) in a dmabuf.

Socket Setup
------------

The user application must use MSG_ZEROCOPY flag when sending devmem TCP. Devmem
cannot be copied by the kernel, so the semantics of the devmem TX are similar
to the semantics of MSG_ZEROCOPY::

	setsockopt(socket_fd, SOL_SOCKET, SO_ZEROCOPY, &opt, sizeof(opt));

It is also recommended that the user binds the TX socket to the same interface
the dma-buf has been bound to via SO_BINDTODEVICE::

	setsockopt(socket_fd, SOL_SOCKET, SO_BINDTODEVICE, ifname, strlen(ifname) + 1);


Sending data
------------

Devmem data is sent using the SCM_DEVMEM_DMABUF cmsg.

The user should create a msghdr where,

* iov_base is set to the offset into the dmabuf to start sending from
* iov_len is set to the number of bytes to be sent from the dmabuf

The user passes the dma-buf id to send from via the dmabuf_tx_cmsg.dmabuf_id.

The example below sends 1024 bytes from offset 100 into the dmabuf, and 2048
from offset 2000 into the dmabuf. The dmabuf to send from is tx_dmabuf_id::

       char ctrl_data[CMSG_SPACE(sizeof(struct dmabuf_tx_cmsg))];
       struct dmabuf_tx_cmsg ddmabuf;
       struct msghdr msg = {};
       struct cmsghdr *cmsg;
       struct iovec iov[2];

       iov[0].iov_base = (void*)100;
       iov[0].iov_len = 1024;
       iov[1].iov_base = (void*)2000;
       iov[1].iov_len = 2048;

       msg.msg_iov = iov;
       msg.msg_iovlen = 2;

       msg.msg_control = ctrl_data;
       msg.msg_controllen = sizeof(ctrl_data);

       cmsg = CMSG_FIRSTHDR(&msg);
       cmsg->cmsg_level = SOL_SOCKET;
       cmsg->cmsg_type = SCM_DEVMEM_DMABUF;
       cmsg->cmsg_len = CMSG_LEN(sizeof(struct dmabuf_tx_cmsg));

       ddmabuf.dmabuf_id = tx_dmabuf_id;

       *((struct dmabuf_tx_cmsg *)CMSG_DATA(cmsg)) = ddmabuf;

       sendmsg(socket_fd, &msg, MSG_ZEROCOPY);


Reusing TX dmabufs
------------------

Similar to MSG_ZEROCOPY with regular memory, the user should not modify the
contents of the dma-buf while a send operation is in progress. This is because
the kernel does not keep a copy of the dmabuf contents. Instead, the kernel
will pin and send data from the buffer available to the userspace.

Just as in MSG_ZEROCOPY, the kernel notifies the userspace of send completions
using MSG_ERRQUEUE::

        int64_t tstop = gettimeofday_ms() + waittime_ms;
        char control[CMSG_SPACE(100)] = {};
        struct sock_extended_err *serr;
        struct msghdr msg = {};
        struct cmsghdr *cm;
        int retries = 10;
        __u32 hi, lo;

        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);

        while (gettimeofday_ms() < tstop) {
                if (!do_poll(fd)) continue;

                ret = recvmsg(fd, &msg, MSG_ERRQUEUE);

                for (cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
                        serr = (void *)CMSG_DATA(cm);

                        hi = serr->ee_data;
                        lo = serr->ee_info;

                        fprintf(stdout, "tx complete [%d,%d]\n", lo, hi);
                }
        }

After the associated sendmsg has been completed, the dmabuf can be reused by
the userspace.


Implementation & Caveats
========================

Unreadable skbs
---------------

Devmem payloads are inaccessible to the kernel processing the packets. This
results in a few quirks for payloads of devmem skbs:

- Loopback is not functional. Loopback relies on copying the payload, which is
  not possible with devmem skbs.

- Software checksum calculation fails.

- TCP Dump and bpf can't access devmem packet payloads.


Testing
=======

More realistic example code can be found in the kernel source under
``tools/testing/selftests/drivers/net/hw/ncdevmem.c``

ncdevmem is a devmem TCP netcat. It works very similarly to netcat, but
receives data directly into a udmabuf.

To run ncdevmem, you need to run it on a server on the machine under test, and
you need to run netcat on a peer to provide the TX data.

ncdevmem has a validation mode as well that expects a repeating pattern of
incoming data and validates it as such. For example, you can launch
ncdevmem on the server by::

	ncdevmem -s <server IP> -c <client IP> -f <ifname> -l -p 5201 -v 7

On client side, use regular netcat to send TX data to ncdevmem process
on the server::

	yes $(echo -e \\x01\\x02\\x03\\x04\\x05\\x06) | \
		tr \\n \\0 | head -c 5G | nc <server IP> 5201 -p 5201
