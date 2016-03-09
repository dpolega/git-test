
/*
 *  defuse-mod.c - A kernel module to kill fork bombs for project2 step2.
 */
#include <linux/module.h>       /* Needed by all modules */
#include <linux/kernel.h>       /* Needed for KERN_INFO */
#include <linux/init.h>         /* Needed for the macros */
#include <linux/sched.h>        // allows access to task_struct
#include <linux/delay.h>        // allows use of msleep()
#include <linux/kthread.h>      // attempt to avoid implicit macro errors
#include <linux/semaphore.h>    // attempt to avoid down_int errors
#include <linux/list.h>         // used for struct list_head
#include <linux/signal.h>       // used for do_send_sig_info
#include <linux/slab.h>         // added for kmalloc
#include <linux/kfifo.h>        // added for kfifo
#include <linux/spinlock.h>     // added for spinlock
#include <linux/rtmutex.h>      // added for tasklist_lock
#include <linux/pid_namespace.h>        // added for find_task_by_vpid
#include <linux/pid.h>          // added for PIDTYPE_PID
// #include <linux/stop_machine.h> // added for stop_machine()
#include <linux/sema_proj3.h>	// added for __fork_proj3_sem__

// GPL license to get access to certain macros
MODULE_LICENSE("GPL");

// declare global variables to allow start and stop kthread
struct task_struct *task_prod;  // producer thread
struct task_struct *task_cons;  // consumer thread

// declare int for count of elements in a tree
int tcount = 0;

// declare global variable for shared data - fifo queue of pids to kill
// fifo represents a PAGE-SIZE-sized queue
DEFINE_KFIFO(fifo, int, PAGE_SIZE);
DEFINE_SPINLOCK(splock);

// declare global variable - semaphore
// DEFINE_SEMAPHORE(mr_sem);
struct semaphore write;
struct semaphore read;

// function prototype for tree_queue()
int tree_queue(struct task_struct *p);
// function prototype for tree_count()
int tree_count(struct task_struct *p);
// function prototype for count_hops_to_init()
int count_hops_to_init(struct task_struct *p);

/*	producer function - detects fork bombs and saves pids in pidlist
*/
int producer(void *data)
{

    // set the threshold for number of children in fork bomb
    int threshold = 10;

    // declare task struct for iterating thru all processes
    struct task_struct *current_task;

    // ancestor count
    int acount = 0;

    // temp semaphore declaration
    struct semaphore *temp = __get_fork_proj3_sem();
    int retdown;
    
    // decrease semaphore count to 0, so that fork() is needed to wake
    while (temp->count > 0) {
	down_interruptible( temp );
    }

    // read lock
    down_interruptible(&read);

    while (!kthread_should_stop()) {

        // iterate through the processes
        for_each_process(current_task) {

            tcount = 0;

		    // find the number of non-init ancestors
		    acount = count_hops_to_init(current_task);

            // traverse if task has children
		    //  and the group id is different from the session id
            if (!list_empty(&current_task->children)
				&& (int) current_task->pid > 100
				&& pid_nr(task_pgrp(current_task)) != 
				pid_nr(task_session(current_task))
				&& acount > 2) {

				// begin tree count of current task
				tree_count(current_task);

				if (tcount > threshold) {

					// begin queuing current task tree
					tree_queue(current_task);
	
			    	// write lock
			    	up(&write);

	                // break from for_each_process
	                break;
				}
    	        else {
    	        	// reset fifo if threshold is not met
    	            kfifo_reset(&fifo);
                }
            }	// end of loop to iterate thru children
        }	// end of loop to iterate thru all tasks

        // sleep for a while
        msleep(300);
	
		// fork hack - wait until fork semaphore is changed by fork()
		retdown = down_interruptible( temp );

	} // end of infinite while loop

    // call up to let the consumer thread stop
    up(&write);

    // reset kfifo
    INIT_KFIFO(fifo);

    // return 0 for success if we get this far
    return 0;
}                               // end of producer()

/*	consumer function - kills processes with pids in kill q
*/
int consumer(void *data)
{

    // declare temp variables for iterating thru tasks
    struct task_struct *task4;
    unsigned int pid2kill;
    int ret2;
    int pgid = 0;
    int pgid2kill = 0;
    int firsttime = 1;

    // temp semaphore declaration
    struct semaphore *temp3 = __get_fork_proj3_sem();

    // use infinite loop for consumer
    // debug
    //printk(KERN_INFO "-- Cons: entering infinite while loop --\n");

    set_current_state(TASK_INTERRUPTIBLE);
    while (!kthread_should_stop())
    {

        // debug
        //printk(KERN_INFO "-- Cons: in while loop, about to call down() --\n");

        // call down to wait for synch semaphore
        // down_interruptible(&mr_sem);
        down_interruptible(&write);

		// reset firsttime
		firsttime = 1;

        while (!kfifo_is_empty(&fifo))
        {

            // read from queue, one integer at a time
            ret2 = kfifo_out_spinlocked(&fifo, &pid2kill, sizeof(pid2kill),
                                        &splock);

		    // get the task_struct from the pid
            task4 = pid_task(find_vpid((pid_t) pid2kill), PIDTYPE_PID);

		    // get the process group id
		    pgid = (int) pid_nr(task_pgrp(task4));

		    // set pgid2kill if this is the first element in queue
		    if (firsttime) {
				pgid2kill = pgid;
				firsttime = 0;
		    }

            // make sure you found a valid task struct - if not, skip
            if ( task4 == NULL || pid2kill < 40 || pgid != pgid2kill ) {

                printk(KERN_INFO "-- Cons: pid %d is granted a pardon --\n", \
					pid2kill);
            }
            else {

                // process data - kill the pid in pidlist
				kill_pgrp(get_task_pid(task4, PIDTYPE_PID), SIGKILL, 9);
                send_sig(SIGKILL, task4, 0);
                printk(KERN_INFO "-- Cons: killing fork-bomb pid %d --\n",
                	pid2kill);
                // msleep(80);     // wait for process to die
    		
				// decrease semaphore to 5
				while (temp3->count > 5) {
					down_interruptible( temp3 );
				}
            }
        } // end of loop to dequeue pids

		up(&read);

        // sleep for a while
        msleep_interruptible(1000);

        set_current_state(TASK_INTERRUPTIBLE);

    } // end of infinite while loop

    // from sleeping kernel article
    // set state as TASK_RUNNING
    __set_current_state(TASK_RUNNING);

    // return 0 if we get this far
    return 0;

} // end of consumer()

static int __init my_name(void)
{
    // print starting message
    printk(KERN_INFO "\n-- defuse-mod STARTING --\n");

    // initializing semaphores
    sema_init(&write, 0);
    sema_init(&read, 1);

    // create and run kernel thread running "consumer"
    // task_cons = kthread_run(&consumer, (void*) &data, "consumer");
    task_cons = kthread_run(&consumer, NULL, "consumer");
    printk(KERN_INFO "-- defuse-mod consumer thread started --\n");

    // create and run kernel thread running "producer"
    // task_prod = kthread_run(&producer, (void*) &data, "producer");
    task_prod = kthread_run(&producer, NULL, "producer");
    printk(KERN_INFO "-- defuse-mod producer thread started --\n");

    /* 
     * A non 0 return means init_module failed; module can't be loaded.
     */
    return 0;
}

static void __exit my_name_exit(void)
{
    // temp vbls
    int pstopflag = -1;
    int cstopflag = -1;
    // temp semaphore declaration
    struct semaphore *temp2 = __get_fork_proj3_sem();

    // stop the kernel threads started earlier
    // stop the producer thread
    up(temp2);
    up(temp2);
    up(temp2);
    up(temp2);
    up(temp2);

    printk(KERN_INFO "-- stopping defuse-mod producer thread --\n");
    pstopflag = kthread_stop(task_prod);
    msleep(60);               // wait for producer to stop
    // debug
    // printk(KERN_INFO "-- pstopflag = %d --\n", pstopflag);

    // stop the consumer thread
    printk(KERN_INFO "-- stopping defuse-mod consumer thread --\n");
    cstopflag = kthread_stop(task_cons);
    // msleep(6000);                // wait for consumer to stop

    // free the kfifo
    printk(KERN_INFO "-- freeing the kfifo in defuse-mod --\n");
    kfifo_free(&fifo);

    // print end of defuse-mod
    printk(KERN_INFO "-- defuse-mod EXITING --\n\n");
}

int tree_queue(struct task_struct *p)
{

    // declare unsigned int for enqueuing pids
    unsigned int pid2q;

    // declare list for iterating thru children
    struct list_head *pos;

    // iterate through each child and traverse their children
    list_for_each(pos, &p->children)
    {
        tree_queue(list_entry(pos, struct task_struct, sibling));
    }

    // print PID of process being queued to kernel log
    printk(KERN_INFO "Adding to kill queue this pid: %i\n", (int) p->pid);
    //printk(KERN_INFO "and the name is: %s\n", p->comm);

    pid2q = (int) p->pid;
    kfifo_in_spinlocked(&fifo, &pid2q, sizeof(pid2q), &splock);

    return 0;
}

int tree_count(struct task_struct *p) {

    // declare list head for iterating thru children
    struct list_head *pos;

    // iterate through each child and traverse their children
    list_for_each(pos, &p->children) {
        tree_count(list_entry(pos, struct task_struct, sibling));
    }

    tcount++;

    return 0;
}

int count_hops_to_init(struct task_struct *p) {

    int hcount = 0;
    struct task_struct *task;
    for(task = p; task != &init_task; task = task->parent) {
	hcount++;
    }

    return hcount;

}  // end of count_ancestors

module_init(my_name);
module_exit(my_name_exit);
