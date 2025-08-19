#include <linux/mm.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <net/tcp.h>

#define INVALID_LABEL -1
#define MAX_BACKLOG_LEN 100
#define HIST_BUCKETS 6

int backlog[MAX_BACKLOG_LEN][2];
int histogram[HIST_BUCKETS];

struct hera_tcp {
    int backlog_len;
    int hist_limit;
    int bucket_size;
    
    int init_cwnd;
    int cwnd_max;
    int cwnd_min;

    u32 curr_time;
    u32 rtt_curr;
};

static u32 hera_undo_cwnd(struct sock* sk) {
    struct hera_tcp* ca = inet_csk_ca(sk);
    struct tcp_sock *tp = tcp_sk(sk);
    return max(tp->snd_cwnd >> 1, ca->init_cwnd); 
}

static void hera_init(struct sock* sk) {
    struct tcp_sock *tp = tcp_sk(sk);
    struct hera_tcp* ca = inet_csk_ca(sk);

    ca->backlog_len = MAX_BACKLOG_LEN;
    ca->bucket_size = 15;
    ca->hist_limit = 10000;
    ca->init_cwnd = 200;
    ca->cwnd_max = 8000;
    ca->cwnd_min = 20;

}

static void push_front(struct hera_tcp *ca, u32 rtt) {
    int i;
    for (i = ca->backlog_len - 1; i > 0; i--)
        memcpy(backlog[i], backlog[i-1], sizeof(backlog[0]));
    
    backlog[0][0] = rtt;
    backlog[0][1] = INVALID_LABEL;
}

static u64 update_histogram(struct sock *sk, struct hera_tcp *ca, u32 rtt) {
    u64 sum = 0, count = 0;
    
    int i;
    for (i = 0; i < ca->backlog_len; i++) {
        if (!backlog[i][0]) break;
        sum += backlog[i][0];
        count++;
    }

    if (!count) return 0;  // No valid samples
    
    u64 avg_rtt = div64_u64(sum, count);
    int bucket = min(div_u64(avg_rtt, ca->bucket_size), HIST_BUCKETS-1);
    
    histogram[bucket]++;
    u64 total = 0, cumulative = 0;
    
    for (i = 0; i < HIST_BUCKETS; i++) {
        total += histogram[i];
        if (i <= bucket) cumulative += histogram[i];
    }

    if (total >= ca->hist_limit) {
        for (i = 0; i < HIST_BUCKETS; i++)
            histogram[i] = (histogram[i] + 1) >> 1;  // Exponential decay
    }

    return div64_u64(cumulative * 100, total);
}

static void adjust_cwnd(struct sock *sk, u64 alpha, int bucket) {
    struct tcp_sock *tp = tcp_sk(sk);
    struct hera_tcp *ca = inet_csk_ca(sk);
    s32 new_cwnd = tp->snd_cwnd;

    if (bucket < HIST_BUCKETS/2) { 
        new_cwnd += div_u64(alpha * (HIST_BUCKETS/2 - bucket), 10);
    } else {
        new_cwnd -= div_u64(alpha * (bucket - HIST_BUCKETS/2 + 1), 10);
    }

    tp->snd_cwnd = clamp(new_cwnd, ca->cwnd_min, ca->cwnd_max);
}

static void hera_main(struct sock *sk, const struct rate_sample *ack) {
    struct hera_tcp *ca = inet_csk_ca(sk);
    if (tcp_jiffies32 < ca->curr_time + ca->rtt_curr) return;

    ca->rtt_curr = usecs_to_jiffies(ack->rtt_us);
    ca->curr_time = tcp_jiffies32;

    push_front(ca, ack->rtt_us / 1000);
    u64 alpha = update_histogram(sk, ca, ack->rtt_us / 1000);
    
    if (alpha) {
        int bucket = div_u64(ack->rtt_us / 1000, ca->bucket_size);
        adjust_cwnd(sk, alpha, bucket);
    }
}


static struct tcp_congestion_ops heratcp __read_mostly = {
    .flags          = TCP_CONG_NON_RESTRICTED,
    .init           = hera_init,
    .ssthresh       = tcp_reno_ssthresh, 
    .undo_cwnd      = hera_undo_cwnd,
    .cong_control   = hera_main,
    .owner          = THIS_MODULE,
    .name           = "hera", 
};

static int __init heratcp_register(void) {
    BUILD_BUG_ON(sizeof(struct hera_tcp) > ICSK_CA_PRIV_SIZE);

    return tcp_register_congestion_control(&heratcp);
}

static void __exit heratcp_unregister(void) {
    tcp_unregister_congestion_control(&heratcp);
}

module_init(heratcp_register);
module_exit(heratcp_unregister);

MODULE_AUTHOR("Comnets"); 
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Hera Congestion Control");
MODULE_VERSION("1.1");