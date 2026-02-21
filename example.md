# Distributed Twitter Clone: Usage Examples

This document illustrates the expected behavior of the Distributed Twitter Clone using 3 standard test cases. Each scenario demonstrates the terminal output across multiple clients as they interact with the system (logging in, posting, and viewing timelines).

## Test Case 1: Basic Following and Posting Flow

In this scenario, we have two clients interacting: User 1 and User 4.

- **User 1 (t1u1):** Logs in and posts a message.
- **User 4 (t1u4):** Logs in, follows User 1, and eventually receives updates.

### User 1 Terminal

![](images/t1u1.png)

### User 4 Terminal

![](images/t1u4.png)

---

## Test Case 2: Multi-User Interaction Sequence

Here, three users (1, 2, and 3) are interacting in a shared timeline environment, broadcasting messages across the clusters.

- **User 1 (t2u1):** Actively posting and fetching timelines.
- **User 2 (t2u2):** Posting updates and following the discussion.
- **User 3 (t2u3):** Following both and reading the federated timeline.

### User 1 Terminal

![](images/t2u1.png)

### User 2 Terminal

![](images/t2u2.png)

### User 3 Terminal

![](images/t2u3.png)

---

## Test Case 3: Complex Multi-Cluster Flow

This advanced scenario demonstrates how the system handles complex follows and timeline synchronizations across different users in potentially different clusters.

- The timeline streams update dynamically as posts are ingested through RabbitMQ synchronizers and routed to the correct `u_following.txt` files of followers.

### User 1 Terminal

![](images/t3u1.png)

### User 2 Terminal

![](images/t3u2.png)

### User 3 Terminal

![](images/t3u3.png)

### User 5 Terminal

![](images/t3u5.png)

---

_Note: Ensure the coordinator, all 6 server replicas, 6 synchronizer replicas, and the RabbitMQ broker are running as specified in the `README.md` before executing parallel client test cases._
