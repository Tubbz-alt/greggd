/*
 * Inspired by the tcplife tool by Brendan Gregg
 *
 * Copyright (c) 2020 Oak Ridge National Laboratory
 * Copyright (c) 2016 Netflix, Inc.
 * Licensed under the Apache License, Version 2.0 (the "License")
 *
 */

#include <uapi/linux/ptrace.h>
#define KBUILD_MODNAME "foo"
#include <linux/tcp.h>
#include <net/sock.h>
#include <bcc/proto.h>

BPF_HASH(birth, struct sock *, u64);

// separate data structs for ipv4 and ipv6
struct ipv4_data_t {
    u32 pid;
    u32 saddr;
    u32 daddr;
    //u64 ports;
    u16 lport;
    u16 rport;
    u64 rx_b;
    u64 tx_b;
    u64 span_us;
    char comm[TASK_COMM_LEN];
    u32 uid;
};
BPF_PERF_OUTPUT(ipv4_events);

struct ipv6_data_t {
    u32 pid;
    unsigned __int128 saddr;
    unsigned __int128 daddr;
    u64 ports;
    u64 rx_b;
    u64 tx_b;
    u64 span_us;
    char comm[TASK_COMM_LEN];
    u32 uid;
};
BPF_PERF_OUTPUT(ipv6_events);

struct id_t {
    u32 pid;
    char comm[TASK_COMM_LEN];
};
BPF_HASH(whoami, struct sock *, struct id_t);

int kprobe__tcp_set_state(struct pt_regs *ctx, struct sock *sk, int state)
{
    u32 pid = bpf_get_current_pid_tgid() >> 32;
    u32 uid = bpf_get_current_uid_gid();

    // lport is either used in a filter here, or later
    u16 lport = sk->__sk_common.skc_num;

    // destination port, switched to host byte order
    //dport = (dport >> 8) | ((dport << 8) & 0x00FF00);
    u16 dport = sk->__sk_common.skc_dport;
    dport = ntohs(dport);

    /*
     * This tool includes PID and comm context. It's best effort, and may
     * be wrong in some situations. It currently works like this:
     * - record timestamp on any state < TCP_FIN_WAIT1
     * - cache task context on:
     *       TCP_SYN_SENT: tracing from client
     *       TCP_LAST_ACK: client-closed from server
     * - do output on TCP_CLOSE:
     *       fetch task context if cached, or use current task
     */

    // capture birth time
    if (state < TCP_FIN_WAIT1) {
        /*
         * Matching just ESTABLISHED may be sufficient, provided no code-path
         * sets ESTABLISHED without a tcp_set_state() call. Until we know
         * that for sure, match all early states to increase chances a
         * timestamp is set.
         * Note that this needs to be set before the PID filter later on,
         * since the PID isn't reliable for these early stages, so we must
         * save all timestamps and do the PID filter later when we can.
         */
        u64 ts = bpf_ktime_get_ns();
        birth.update(&sk, &ts);
    }

    // record PID & comm on SYN_SENT
    if (state == TCP_SYN_SENT || state == TCP_LAST_ACK) {
        // now we can PID filter, both here and a little later on for CLOSE
        //FILTER_PID
        struct id_t me = {.pid = pid};
        bpf_get_current_comm(&me.comm, sizeof(me.comm));
        whoami.update(&sk, &me);
    }

    if (state != TCP_CLOSE)
        return 0;

    // calculate lifespan
    u64 *tsp, delta_us;
    tsp = birth.lookup(&sk);
    if (tsp == 0) {
        whoami.delete(&sk);     // may not exist
        return 0;               // missed create
    }
    delta_us = (bpf_ktime_get_ns() - *tsp) / 1000;
    birth.delete(&sk);

    // fetch possible cached data, and filter
    struct id_t *mep;
    mep = whoami.lookup(&sk);
    if (mep != 0)
        pid = mep->pid;
    //FILTER_PID

    // get throughput stats. see tcp_get_info().
    u64 rx_b = 0, tx_b = 0, sport = 0;
    struct tcp_sock *tp = (struct tcp_sock *)sk;
    rx_b = tp->bytes_received;
    tx_b = tp->bytes_acked;

    u16 family = sk->__sk_common.skc_family;

    if (family == AF_INET) {
        struct ipv4_data_t data4 = {};
        data4.span_us = delta_us;
        data4.rx_b = rx_b;
        data4.tx_b = tx_b;
        data4.saddr = sk->__sk_common.skc_rcv_saddr;
        data4.daddr = sk->__sk_common.skc_daddr;
        // a workaround until data4 compiles with separate lport/dport
        data4.pid = pid;
        data4.uid = uid;
        data4.lport = lport;
        data4.rport = dport;
        if (mep == 0) {
            bpf_get_current_comm(&data4.comm, sizeof(data4.comm));
        } else {
            bpf_probe_read(&data4.comm, sizeof(data4.comm), (void *)mep->comm);
        }
        ipv4_events.perf_submit(ctx, &data4, sizeof(data4));

    } else /* 6 */ {
        struct ipv6_data_t data6 = {};
        data6.span_us = delta_us;
        data6.rx_b = rx_b;
        data6.tx_b = tx_b;
        bpf_probe_read(&data6.saddr, sizeof(data6.saddr),
            sk->__sk_common.skc_v6_rcv_saddr.in6_u.u6_addr32);
        bpf_probe_read(&data6.daddr, sizeof(data6.daddr),
            sk->__sk_common.skc_v6_daddr.in6_u.u6_addr32);
        // a workaround until data6 compiles with separate lport/dport
        data6.ports = dport + ((0ULL + lport) << 32);
        data6.pid = pid;
        data6.uid = uid;
        if (mep == 0) {
            bpf_get_current_comm(&data6.comm, sizeof(data6.comm));
        } else {
            bpf_probe_read(&data6.comm, sizeof(data6.comm), (void *)mep->comm);
        }
        ipv6_events.perf_submit(ctx, &data6, sizeof(data6));
    }

    if (mep != 0)
        whoami.delete(&sk);

    return 0;
}
