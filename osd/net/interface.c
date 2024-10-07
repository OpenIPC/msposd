#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <netlink/netlink.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/family.h>
#include <netlink/genl/ctrl.h>
#include <linux/nl80211.h>

extern int frequency;

static int print_handler(struct nl_msg *msg, void *arg) {
    struct nlattr *tb_msg[NL80211_ATTR_MAX + 1];
    struct genlmsghdr *gnlh = nlmsg_data(nlmsg_hdr(msg));

    nla_parse(tb_msg, NL80211_ATTR_MAX, genlmsg_attrdata(gnlh, 0),
              genlmsg_attrlen(gnlh, 0), NULL);

    if (tb_msg[NL80211_ATTR_WIPHY_FREQ]) {
        int freq = nla_get_u32(tb_msg[NL80211_ATTR_WIPHY_FREQ]);
        frequency = freq;
        printf("Frequency on interface: %d MHz\n", freq);
    }

    return NL_OK;
}

int freq_to_channel(int freq) {
    if (freq >= 2412 && freq <= 2472)
        return (freq - 2407) / 5;
    else if (freq == 2484)
        return 14;
    else if (freq >= 5180 && freq <= 5825)
        return (freq - 5000) / 5;
    else
        return -1; // Unknown frequency
}

int read_current_freq_from_interface() {
    struct nl_sock *sock;
    struct nl_msg *msg;
    int driver_id;
    int if_index = if_nametoindex("wlan0");

    if (if_index == 0) {
        perror("Unable to find interface");
        return -1;
    }

    // Allocate new netlink socket
    sock = nl_socket_alloc();
    if (!sock) {
        perror("Unable to allocate socket");
        return -1;
    }

    // Connect to generic netlink socket
    if (genl_connect(sock)) {
        perror("Unable to connect to generic netlink");
        nl_socket_free(sock);
        return -1;
    }

    // Resolve nl80211 driver ID
    driver_id = genl_ctrl_resolve(sock, "nl80211");
    if (driver_id < 0) {
        perror("nl80211 not found");
        nl_socket_free(sock);
        return -1;
    }

    // Allocate message and set family
    msg = nlmsg_alloc();
    if (!msg) {
        perror("Unable to allocate message");
        nl_socket_free(sock);
        return -1;
    }

    genlmsg_put(msg, 0, 0, driver_id, 0, 0, NL80211_CMD_GET_INTERFACE, 0);

    nla_put_u32(msg, NL80211_ATTR_IFINDEX, if_index);

    // Send the message and handle the response
    nl_socket_modify_cb(sock, NL_CB_VALID, NL_CB_CUSTOM, print_handler, NULL);

    if (nl_send_auto(sock, msg) < 0) {
        perror("Error sending message");
        nlmsg_free(msg);
        nl_socket_free(sock);
        return -1;
    }

    // Wait for the result
    nl_recvmsgs_default(sock);

    // Clean up
    nlmsg_free(msg);
    nl_socket_free(sock);

    return 0;
}