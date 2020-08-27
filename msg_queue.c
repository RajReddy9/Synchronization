/*
 * This code is provided solely for the personal and private use of students
 * taking the CSC369H course at the University of Toronto. Copying for purposes
 * other than this use is expressly prohibited. All forms of distribution of
 * this code, including but not limited to public repositories on GitHub,
 * GitLab, Bitbucket, or any other online platform, whether as given or with
 * any changes, are expressly prohibited.
 *
 * Authors: Alexey Khrabrov, Andrew Pelegris, Karen Reid
 *
 * All of the files in this directory and all subdirectories are:
 * Copyright (c) 2019 Karen Reid
 */

/**
 * CSC369 Assignment 2 - Message queue implementation.
 *
 * You may not use the pthread library directly. Instead you must use the
 * functions and types available in sync.h.
 */

#include <assert.h>
#include <errno.h>
#include <stdlib.h>

#include "errors.h"
#include "list.h"
#include "msg_queue.h"
#include "ring_buffer.h"

/** A read operation. */
#define READ 1

/** A read operation. */
#define WRITE 2

/** A close operation. */
#define CLOSE 3

/** Stage1 indicator. */
#define STAGE1 1

/** Stage3 indicator. */
#define STAGE3 3

// Subscription data of the queue handles.
typedef struct sub_data
{
	// An entry in the doubly-linked list which can be used to traverse in to
	// check if the events actually happens.
    list_entry list_entry;

	// Pointer to the conditional variable that poll waits on
    cond_t* have_event;

	// Pointer to the poll mutex 
	mutex_t* poll_mutex;

	// Pointer to the event_flag which will be set to one if a required events
	// happen.
	int* event_flag;

	// Events that subscribed to
    int events;

	// Queue handle that has the subscribed events
	msg_queue_t queue;
}sub_data;

// Message queue implementation backend
typedef struct mq_backend {
	// Ring buffer for storing the messages
	ring_buffer buffer;

	// Reference count
	size_t refs;

	// Number of handles open for reads
	size_t readers;
	// Number of handles open for writes
	size_t writers;

	// Set to true when all the reader handles have been closed. Starts false
	// when they haven't been opened yet.
	bool no_readers;
	// Set to true when all the writer handles have been closed. Starts false
	// when they haven't been opened yet.
	bool no_writers;
    
	//TODO - Add the necessary synchronization variables.

	// Lock that used for sychronization in read, write and poll.
	mutex_t lock;

	// If a write happens, broadcast the conditional variable not_empty because
	// there are something in the buffer that makes it no longer empty.
    cond_t not_empty;

	// If a read happens, broadcast the conditional variable not_full because 
	// the buffer is no longer full.
    cond_t not_full;
    
	// The list_head of the doubly linked list which is contained in sub_data. 
	// The head in root is connected to itself if there are no queue handles 
	// related to the buffer in this mq_backend.
    list_head root;
    
} mq_backend;

/**
 * Signal the conditional variable have_event if specific conditions are satisfied.
 * 
 * operation is defined in msg_queue.h to clarify different conditions we need after 
 * a read, write or close happen.
 * 
 * @param mq		pointer to the message queue implementation backend.
 * @param operation	one of READ, WRITE and CLOSE. Indicating different conditions we
 *					need to satisfy.	     
 */
void waken(mq_backend* mq, int operation);

static int mq_init(mq_backend *mq, size_t capacity)
{
	if (ring_buffer_init(&mq->buffer, capacity) < 0) return -1;

	mq->refs = 0;

	mq->readers = 0;
	mq->writers = 0;

	mq->no_readers = false;
	mq->no_writers = false;

	//TODO

    list_init(&(mq->root));
    
	mutex_init(&(mq->lock));
    
    cond_init(&(mq->not_empty));
    
    cond_init(&(mq->not_full));
	
	return 0;
}

static void mq_destroy(mq_backend *mq)
{
	assert(mq->refs == 0);
	assert(mq->readers == 0);
	assert(mq->writers == 0);
    
    
	//TODO
    mutex_destroy(&(mq->lock));
    cond_destroy(&(mq->not_empty));
    cond_destroy(&(mq->not_full));
	ring_buffer_destroy(&mq->buffer);
}


#define ALL_FLAGS (MSG_QUEUE_READER | MSG_QUEUE_WRITER | MSG_QUEUE_NONBLOCK)

// Message queue handle is a combination of the pointer to the queue backend and
// the handle flags. The pointer is always aligned on 8 bytes - its 3 least
// significant bits are always 0. This allows us to store the flags within the
// same word-sized value as the pointer by ORing the pointer with the flag bits.

// Get queue backend pointer from the queue handle
static mq_backend *get_backend(msg_queue_t queue)
{
	mq_backend *mq = (mq_backend*)(queue & ~ALL_FLAGS);
	assert(mq);
	return mq;
}

// Get handle flags from the queue handle
static int get_flags(msg_queue_t queue)
{
	return (int)(queue & ALL_FLAGS);
}

// Create a queue handle for given backend pointer and handle flags
static msg_queue_t make_handle(mq_backend *mq, int flags)
{
	assert(((uintptr_t)mq & ALL_FLAGS) == 0);
	assert((flags & ~ALL_FLAGS) == 0);
	return (uintptr_t)mq | flags;
}


static msg_queue_t mq_open(mq_backend *mq, int flags)
{
	++mq->refs;

	if (flags & MSG_QUEUE_READER) {
		++mq->readers;
		mq->no_readers = false;
	}
	if (flags & MSG_QUEUE_WRITER) {
		++mq->writers;
		mq->no_writers = false;
	}

	return make_handle(mq, flags);
}

// Returns true if this was the last handle
static bool mq_close(mq_backend *mq, int flags)
{
	assert(mq->refs != 0);
	assert(mq->refs >= mq->readers);
	assert(mq->refs >= mq->writers);

	if (flags & MSG_QUEUE_READER) {
		if (--mq->readers == 0) mq->no_readers = true;
	}
	if (flags & MSG_QUEUE_WRITER) {
		if (--mq->writers == 0) mq->no_writers = true;
	}

	if (--mq->refs == 0) {
		assert(mq->readers == 0);
		assert(mq->writers == 0);
		return true;
	}
	return false;
}


msg_queue_t msg_queue_create(size_t capacity, int flags)
{
	if (flags & ~ALL_FLAGS) {
		errno = EINVAL;
		report_error("msg_queue_create");
		return MSG_QUEUE_NULL;
	}

	mq_backend *mq = (mq_backend*)malloc(sizeof(mq_backend));
	if (mq == NULL) {
		report_error("malloc");
		return MSG_QUEUE_NULL;
	}
	// Result of malloc() is always aligned on 8 bytes, allowing us to use the
	// 3 least significant bits of the handle to store the 3 bits of flags
	assert(((uintptr_t)mq & ALL_FLAGS) == 0);

	if (mq_init(mq, capacity) < 0) {
		// Preserve errno value that can be changed by free()
		int e = errno;
		free(mq);
		errno = e;
		return MSG_QUEUE_NULL;
	}

	return mq_open(mq, flags);
}

msg_queue_t msg_queue_open(msg_queue_t queue, int flags)
{
	if (!queue) {
		errno = EBADF;
		report_error("msg_queue_open");
		return MSG_QUEUE_NULL;
	}

	if (flags & ~ALL_FLAGS) {
		errno = EINVAL;
		report_error("msg_queue_open");
		return MSG_QUEUE_NULL;
	}

	mq_backend *mq = get_backend(queue);

	//TODO
	mutex_lock(&(mq->lock));
	// Enter critical section.
	msg_queue_t new_handle = mq_open(mq, flags);
	// End of critical section.
	mutex_unlock(&(mq->lock));

	return new_handle;
}

int msg_queue_close(msg_queue_t *queue)
{
	if (!*queue) {
		errno = EBADF;
		report_error("msg_queue_close");
		return -1;
	}

	mq_backend *mq = get_backend(*queue);

    mutex_lock(&(mq->lock));
	// enter critical section.

	if (mq_close(mq, get_flags(*queue))) {
		// If root.head has entries linked to it, then we need to
		// call waken to loop through the entries. 
		if (mq->root.head.next != &(mq->root.head)) {
			waken(mq, CLOSE);
		}
		// Closed last handle; destroy the queue
        mutex_unlock(&(mq->lock));
		mq_destroy(mq);
		free(mq);
		*queue = MSG_QUEUE_NULL;
		return 0;
	}
	// no readers means that we can broadcast the cv not_full so that write 
	// thread can start writing.
	if (mq->no_readers) {
		cond_broadcast(&(mq->not_full));
	}
	// no writers means that we can broadcast the cv not_empty so that read 
	// thread can start reading.
	if (mq->no_writers) {
		cond_broadcast(&(mq->not_empty));
	}
	// if mq->root.head has entries linked to it, then we need to
	// call waken to loop through the entries. 
	if (mq->root.head.next != &(mq->root.head)) {
			waken(mq, CLOSE);
		}

	// end of critical section.
    mutex_unlock(&(mq->lock));
	
	*queue = MSG_QUEUE_NULL;
	return 0;
}


ssize_t msg_queue_read(msg_queue_t queue, void *buffer, size_t length)
{
	//TODO
    // error check for queue NULL
	if (!queue) {
		errno = EBADF;
		report_error("msg_queue_read");
		return -1;
	}
    // get mq_backend
    mq_backend *mq = get_backend(queue);

    mutex_lock(&(mq->lock));
	// enter critical section

    // backend check no writer and no data in buffer
	if (ring_buffer_used(&(mq->buffer)) == 0 && mq->no_writers) {
		mutex_unlock(&(mq->lock));
		return 0;
	}

    // wait if there is no data in the ring_buffer.
    while(ring_buffer_used(&(mq->buffer)) == 0) {
		// if the flag is nonblocking we should report error since we are not 
		// suppose to wait for the data.
		if (get_flags(queue) & MSG_QUEUE_NONBLOCK) {
			errno = EAGAIN;
			report_error("msg_queue_read");
			mutex_unlock(&(mq->lock));
			return -1;
		}
		// wait for not_empty which means there are some data in the ring buffer.
		cond_wait(&(mq->not_empty),&(mq->lock));
	}

    // use peek to get the message length.
    size_t msg_length;
	if (!ring_buffer_peek(&(mq->buffer), &msg_length, sizeof(size_t))) {
		mutex_unlock(&(mq->lock));
		return -1;
	}
	// if message length is larger then the length of buffer, report error.
	if (msg_length > length) {
		errno = EMSGSIZE;
		report_error("msg_queue_read");
		mutex_unlock(&(mq->lock));
		return -msg_length;
	} else { // otherwise use read to read out a message.
        // read message length again to make the tail point to the start of
		// message.
		if (!ring_buffer_read(&(mq->buffer), &msg_length, sizeof(size_t)))
        {
			mutex_unlock(&(mq->lock));
			return -1;
		}
        // read message.
		if (!ring_buffer_read(&(mq->buffer), buffer, msg_length))
        {
			mutex_unlock(&(mq->lock));
			return -1;
		}
		
	}
    
    // read success and we can broadcast not_full indicating that ring_buffer is
	// no longer full.
    cond_broadcast(&(mq->not_full));

    // if root is only itialized without any insertation, return directly.
    if(mq->root.head.next == &(mq->root.head)) {	
        mutex_unlock(&(mq->lock));
        return msg_length;
    }
    
    // otherwise use waken to traverse throught the list entries and send the signal
	// for stage 2 of poll.
	waken(mq, READ);
	
	// end of critical section.
	mutex_unlock(&(mq->lock));

	return msg_length;
}

int msg_queue_write(msg_queue_t queue, const void *buffer, size_t length)
{
	//TODO
    // error check for queue NULL.
	if (!queue) {
		errno = EBADF;
		report_error("msg_queue_write");
		return -1;
	}
    // get mq_backend.
    mq_backend *mq = get_backend(queue);

	mutex_lock(&(mq->lock));
	// Enter critical section.

    // check for invalid write.
	if (length == 0) {
		errno = EINVAL;
		report_error("msg_queue_write");
        mutex_unlock(&(mq->lock));
		return -1;
	}

    // check if all the read handles have been closed and report error.
	if (mq->no_readers) {
		errno = EPIPE;
		report_error("msg_queue_write");
		mutex_unlock(&(mq->lock));
		return -1;
	}
	// check if the buffer size if smaller then the messsage size and report error.
    else if ((mq->buffer).size < length + sizeof(size_t)) {
		errno = EMSGSIZE;
		mutex_unlock(&(mq->lock));
		return -1;
	}
    
	// wait if there is not enough space in mq->buffer for holding the message.
	while (ring_buffer_free(&(mq->buffer)) < length + sizeof(size_t)) {
		// if the flag is nonblocking we should report error since we are not 
		// suppose to wait for the buffer to have space.
		if (get_flags(queue) & MSG_QUEUE_NONBLOCK){
			errno = EAGAIN;
			report_error("msg_queue_write\n");
			mutex_unlock(&(mq->lock));
			return -1;
		}
		// wait for not_full which means there are some space to write.
		cond_wait(&(mq->not_full),&(mq->lock));
	}

    // write the length of the message.
	if (!ring_buffer_write(&(mq->buffer), &length, sizeof(size_t))) {
		// return -1 for error.
		mutex_unlock(&(mq->lock));
		return -1;
	}
    
	// write the message.
	if (!ring_buffer_write(&(mq->buffer), buffer, length)) {
		//return -1 for error.
		mutex_unlock(&(mq->lock));
		return -1;
	}
    
	// broadcast not_empty.
    cond_broadcast(&(mq->not_empty));
    
    // if root is only initialized without insertation, return directly.
    if(mq->root.head.next == &(mq->root.head)) {
        mutex_unlock(&(mq->lock));
        return 0;
    } 
    // otherwise use waken to traverse throught the list entries and send the signal
	// for stage 2 of poll.
	waken(mq, WRITE);
	
	// end of critical section.
	mutex_unlock(&(mq->lock));
	return 0;
}
// Help function to send the signal for stage 2 and notify poll to get into stage 3, 
// should be called after each queue has some changes.
void waken(mq_backend* mq, int operation)
{
    // get current queue root
	list_head* root = &(mq->root);
    list_entry* current = NULL;
    // traverse the element in this queue root list.
    list_for_each(current,root) {
        // get embemed struct
        sub_data* data = container_of(current, struct sub_data, list_entry);
        // Null checker
		if (data) {
            // get lock since we need use conditional variable have_event
			mutex_lock(data->poll_mutex);
			// enter critical section.

            // case of MSG_QUEUE_NULL should be ignored
			if (data->queue != MSG_QUEUE_NULL) {
				// if a close happens.
				if (operation == CLOSE) {
					// if we subscribe to MQPOLL_WRITABLE and there are no readers,
					// then MQPOLL_NOREADERS could happen. 
					if ((data->events & MQPOLL_WRITABLE) && mq->no_readers) {
						// some event triggers.
						cond_signal(data->have_event);
						// change the event flag to break the while loop.
						*(data->event_flag) = 1;
					}
					// if we subscribe to MQPOLL_READABLE and there are no writers,
					// then MQPOLL_NOWRITERS could happen
					if ((data->events & MQPOLL_READABLE) && mq->no_writers) {
						cond_signal(data->have_event);
						*(data->event_flag) = 1;
					}
					// if we subscribe to MQPOLL_NOWRITERS and there are no writers,
					// then MQPOLL_NOWRITERS could happen. 
					if ((data->events & MQPOLL_NOWRITERS) && mq->no_writers) {
						cond_signal(data->have_event);
						*(data->event_flag) = 1;
					}
					// if we subscribe to MQPOLL_NOREADERS and there are no readers,
					// then MQPOLL_NOREADERS could happen.
					if ((data->events & MQPOLL_NOREADERS) && mq->no_readers) {
						cond_signal(data->have_event);
						*(data->event_flag) = 1;
					}
					
				}	
				// if a read happens.
				if (operation == READ) {
					// if we subscribe to MQPOLL_WRITABLE and there are some space in mq->buffer,
					// then MQPOLL_WRITABLE could happen. 
					if((data->events & MQPOLL_WRITABLE) && ring_buffer_free(&(mq->buffer))>sizeof(size_t)) {	
						cond_signal(data->have_event);
						*(data->event_flag) = 1;
					}
				}
				// if a write happens.
				if (operation == WRITE) {
					// if we subscribe to MQPOLL_WRITABLE and there are some space in mq->buffer,
					// then MQPOLL_WRITABLE could happen.
					if((data->events & MQPOLL_READABLE) && ring_buffer_used(&(mq->buffer))) {	
						cond_signal(data->have_event);
						*(data->event_flag) = 1;
					}
				}
			}
            // end of critical section.
			mutex_unlock(data->poll_mutex);
		}
    }
}

/**
 * Count the number of queue that are ready for I/O and set the revent of fds.
 * 
 * For out poll implementation, since we are going through similar process in stage 1 and stage 3,
 * we combine the two process to this ready_queue_set function which will go through different 
 * part of code based on stage.
 * @param nfds			number of items in the fds array.
 * @param fds			message queues to be monitored.
 * @param poll_mutex	pointer to the poll_mutex initialized in poll.
 * @param have_event	pointer to the conditional variable initialized in poll.
 * @param event_flag	pointer to the int that is the while condition for stage 2.
 * @param set			pointer to the starter of set of sub_datas in poll.
 * @param stage			one of STAGE1 and STAGE3
 * @return 				number of queues ready for I/O (with nonzero revents) on success;
 *             			-1 on failure (with errno set).
 */
int ready_queue_set(size_t nfds, msg_queue_pollfd *fds, mutex_t* poll_mutex, cond_t* have_event,int* event_flag, sub_data* set, int stage)
{
	// number of revent counter
	int current_event = 0;
	for(size_t i=0;i<nfds;++i){
        // MSG_QUEUE_NULL check
		if(fds[i].queue == MSG_QUEUE_NULL) {
			fds[i].revents = 0;
			continue;
		} 
        
	    // get the backend
		mq_backend* mq = get_backend(fds[i].queue);
      
        mutex_lock(&(mq->lock));
        mutex_lock(poll_mutex);
		// enter critical section.

		if (stage == STAGE1) {
			// initialize all the variables in struct sub_data.
			set[i].have_event = have_event;
			set[i].poll_mutex = poll_mutex;
			set[i].event_flag = event_flag;
			set[i].events = fds[i].events;
			set[i].queue = fds[i].queue;

			// get the flags for this queue.
			int mq_flag = get_flags((fds+i)->queue);
			// a reader handle could subscribe to MQPOLL_NOWRITERS.
			if (mq_flag & MSG_QUEUE_READER) set[i].events |= MQPOLL_NOWRITERS;
			// a writer handle could subscribe to MQPOLL_NOREADERS.
			if (mq_flag & MSG_QUEUE_WRITER) set[i].events |= MQPOLL_NOREADERS;
		
			// error check for EINVAL.
			// if events is not 0 and is not any of the 4 flags given, report error. 
			if(fds[i].events != 0 && !((fds[i].events & MQPOLL_READABLE) || (fds[i].events & MQPOLL_WRITABLE) 
				|| (fds[i].events & MQPOLL_NOREADERS) || (fds[i].events & MQPOLL_NOWRITERS))) {
				errno = EINVAL;
				report_error("msg_queue_poll");
				mutex_unlock(poll_mutex);
				mutex_unlock(&(mq->lock));
				return -1;
			}
			
			// if MQPOLL_READABLE requested for a non-reader queue handle, report error.
			if((fds[i].events & MQPOLL_READABLE) && !((mq_flag & MSG_QUEUE_READER))) {
				errno = EINVAL;
				report_error("msg_queue_poll");
				mutex_unlock(poll_mutex);
				mutex_unlock(&(mq->lock));
				return -1;
			}
			
			// if MQPOLL_WRITABLE requested for a non-writer queue handle, report error.
			if((fds[i].events & MQPOLL_WRITABLE) && !(mq_flag & MSG_QUEUE_WRITER)) {
				errno = EINVAL;
				report_error("msg_queue_poll");
				mutex_unlock(poll_mutex);
				mutex_unlock(&(mq->lock));
				return -1;
			}
		}

		// flag is 0 initially and will be set to 1 if there are revents in stage 1. 
		int flag = 0;

        // Case 1. If events is readable, then we can set revent if either there is 
		// a message to be read, or all the writer handles to the queue have been closed.
        if(set[i].events & MQPOLL_READABLE) {
			if (ring_buffer_used(&(mq->buffer)) || mq->no_writers){
				fds[i].revents |= MQPOLL_READABLE;
				flag = 1;
			}
			// set the revents back to 0 if it does not satisfy the condition for readable.
			if (!flag) {
				fds[i].revents = 0;
			}
        }

        // Case 2. If events is writable, then we can set revent if either there is 
		// some space to write, or all the reader handles to the queue have been closed.
        if(set[i].events & MQPOLL_WRITABLE) {
            if (ring_buffer_free(&(mq->buffer))>sizeof(size_t) || mq->no_readers) {
				fds[i].revents |= MQPOLL_WRITABLE;
				flag = 1;
			}
			// set the revents back to 0 if it does not satisfy the condition for readable.
			if (!flag) {
				fds[i].revents = 0;
			}
        }

        // Case 3. If events is MQPOLL_NOREADERS, then we can set revent if there is no
		// readers in the backend.
        if((set[i].events & MQPOLL_NOREADERS) && mq->no_readers) {
            fds[i].revents |= MQPOLL_NOREADERS;
			flag = 1;
        }

        // Case 4. If events is MQPOLL_NOWRITERS, then we can set revent if there is no
		// writers in the backend.
        if((set[i].events & MQPOLL_NOWRITERS) && mq->no_writers){
            fds[i].revents |= MQPOLL_NOWRITERS;
			flag = 1;
        }
        
		// increment current_event if revent is set and flag changes to 1.
		current_event += flag;
		if (stage == STAGE1) {
			list_add_tail(&(mq->root),&(set[i].list_entry));
		}
		if (stage == STAGE3) {
			// Delete list from entry
        	list_del(&(mq->root), &(set[i].list_entry));
		}

		// end of critical section.
        mutex_unlock(poll_mutex);
        mutex_unlock(&(mq->lock));
    }
	return current_event;
} 

int msg_queue_poll(msg_queue_pollfd *fds, size_t nfds)
{
	// check if nfds is 0 and report error.
    if(!nfds) {
        errno = EINVAL;
		report_error("msg_queue_poll");
        return -1;
    }
    // mutex lock for poll
    mutex_t poll_mutex;
	// initialize the poll_lock
    mutex_init(&poll_mutex);

    // condition variable for poll
    cond_t have_event;
	//initialize the conditional variable
    cond_init(&have_event);
    
    // use event_flag to break the while loop in stage 2.
	int event_flag = 0;

	// struct to contain link list
    sub_data set[nfds];

    int res = ready_queue_set(nfds, fds, &poll_mutex, &have_event, &event_flag, set, STAGE1);
    // Stage 1 return
	if (res == -1){
		return -1;
	}else if (res) {
		for(size_t i=0;i<nfds;++i){
			// Ignore all MSG_QUEUE_NULL
			if(fds[i].queue == MSG_QUEUE_NULL) {
				fds[i].revents = 0;
				continue;
			}
			// get current backend queue
			mq_backend* mq = get_backend(fds[i].queue);
			mutex_lock(&(mq->lock));
			mutex_lock(&poll_mutex);
			// enter critical section.
			// delete the list entry.
			list_del(&(mq->root), &(set[i].list_entry));
			// end of critical section.
			mutex_unlock(&poll_mutex);
			mutex_unlock(&(mq->lock));
		}
		// destroy the conditional variable
		cond_destroy(&have_event);
		// destroy the mutex lock
		mutex_destroy(&poll_mutex);
		return res;
	} 

    // Start of stage 2
	mutex_lock(&poll_mutex);
	// enter critical section.
	// wait until the event_flag is set to 1 by some queue handles running waken.
	while (!event_flag) {
		// wait for the have_event being signal by some threads.
		cond_wait(&have_event,&poll_mutex);
	}
	// end of critical section.
	mutex_unlock(&poll_mutex);

    // Start of stage 3.
	res = ready_queue_set(nfds, fds, &poll_mutex, &have_event, &event_flag, set, STAGE3);
    // destroy the conditional variable
    cond_destroy(&have_event);
	// destroy the mutex lock
    mutex_destroy(&poll_mutex);
	
	return res;
}
