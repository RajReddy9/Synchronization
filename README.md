# Synchronization
## Introduction
In this assignment you will complete an implementation of a message queue by adding the necessary locks and condition variable operations to synchronize access to the message queue. You will also implement I/O multiplexing functionality - monitoring multiple queues for events (e.g. new messages) in a single thread - modeled after the `poll()` system call.

## Part 1: Message queue implementation
A message queue is very similar to a pipe. The key difference is that a pipe is a stream of bytes, and a message queue stores distinct messages by first storing the message length (as a `size_t` type), and then the message itself. This means that a receiver retrieves a message by first reading the `size` of the message, and then reading `size` bytes from the message queue.

Your first task is to complete the functions in `msg_queue.c` with the exception of `msg_queue_poll()`. The description of each function is in `msg_queue.h`. Plan to spend some time studying these two files so that you understand the underlying implementation of the message queue.

For example, most of the `msg_queue_create()`, `msg_queue_open()`, and `msg_queue_close()` functions are given to you. Notice that `msg_queue_create()` allocates space for and initializes a data structure (`mq_backend`) that maintains all the state for the queue. You will need to add your synchronization variables here. Also, notice that `msg_queue_create()` returns a message queue handle (much like a file descriptor). Trace through the code to see how this handle is created and returned.

The starter code also includes a ring buffer (aka circular buffer) to store the messages. The file `ring_buffer.h` describes the functions used to read and write from the buffer as well as to check how much space is used or free.

## Part 2: Implementing msg_queue_poll()
The task of `msg_queue_poll()` is to wait until one of a set of multiple queues becomes ready for I/O (read or write). The API that you need to implement is modeled after the `poll()` system call, so please read the manual page for it (man 2 poll). Note that you do not need to implement the timeout feature of the Linux poll. We will provide a simple test program that uses `msg_queue_poll()`.

Your `msg_queue_poll()` function should consist of 3 stages:

1. Subscribe to requested events. `msg_queue_poll` takes and array of `msg_queue_pollfd`. This array contains information about the message queues that poll call will be monitoring including the events that a thread has requested.
2. Check if any of the requested events on any of the queues have already been triggered: If none of the requested events are already triggered, block until another thread triggers an event. Blocking should be implemented by waiting on a condition variable (which will be signaled by the thread that triggers the event). Note that you will need to keep track of whether an event has been signalled and which thread to wake up when an event is signalled.
3. After the blocking wait is complete, determine what events have been triggered, and return the number of message queues with any triggered events. Like poll and select, the caller will then check each message queue it is subscribed to for an event.
