/*
 * Copyright (C) 2016 Tamas K Lengyel
 * ACADEMIC PUBLIC LICENSE
 * For details please read the LICENSE file.
 */

/*
 * gcc ept.c `pkg-config --cflags --libs glib-2.0 libvmi` -o ept
 */
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <libvmi/libvmi.h>
#include <libvmi/events.h>
#include <libvmi/libvmi_extra.h>
#include <libvmi/x86.h>

#define KERNEL_VADDR 0x80000000

static status_t status;

/* Signal handler */
static struct sigaction act;
static int interrupted = 0;
static void close_handler(int sig){
    interrupted = sig;
}

vmi_event_t singlestep_event;
GSList *events = NULL;

event_response_t singlestep_cb(vmi_instance_t vmi, vmi_event_t *event) {
    vmi_register_event(vmi, event->data);
    return (1u << VMI_EVENT_RESPONSE_TOGGLE_SINGLESTEP);
}

static inline
void print_event(vmi_event_t *event){
    printf("\tPAGE %"PRIx64" ACCESS: %c%c%c for GFN %"PRIx64" (offset %06"PRIx64") gla %016"PRIx64" (vcpu %u)\n",
        event->mem_event.physical_address,
        (event->mem_event.out_access & VMI_MEMACCESS_R) ? 'r' : '-',
        (event->mem_event.out_access & VMI_MEMACCESS_W) ? 'w' : '-',
        (event->mem_event.out_access & VMI_MEMACCESS_X) ? 'x' : '-',
        event->mem_event.gfn,
        event->mem_event.offset,
        event->mem_event.gla,
        event->vcpu_id
    );
}

event_response_t trap_cb(vmi_instance_t vmi, vmi_event_t *event) {

    //print_event(event);

    // TODO 2: read a string out from the currently accessed memory location and print it

    vmi_clear_event(vmi, event, NULL);
    singlestep_event.data = event;
    return (1u << VMI_EVENT_RESPONSE_TOGGLE_SINGLESTEP);
}

int main(int argc, char **argv) {

    if (argc!=3) {
        printf("Usage: %s <domain name> <pid>\n", argv[0]);
        return 1;
    }

    const char *name = argv[1];
    vmi_pid_t pid = atoi(argv[2]);

    /* Signal handler to catch CTRL+C, etc.. */
    act.sa_handler = close_handler;
    act.sa_flags = 0;
    sigemptyset(&act.sa_mask);
    sigaction(SIGHUP,  &act, NULL);
    sigaction(SIGTERM, &act, NULL);
    sigaction(SIGINT,  &act, NULL);
    sigaction(SIGALRM, &act, NULL);

    vmi_instance_t vmi;
    // Initialize the libvmi library.
    if (vmi_init(&vmi, VMI_XEN | VMI_INIT_COMPLETE | VMI_INIT_EVENTS, name) == VMI_FAILURE) {
        printf("Failed to init LibVMI library.\n");
        return;
    }

    addr_t dtb = vmi_pid_to_dtb(vmi, pid);

    vmi_pause_vm(vmi);

    SETUP_SINGLESTEP_EVENT(&singlestep_event, 1, singlestep_cb, 0);
    vmi_register_event(vmi, &singlestep_event);

    GSList *pages = vmi_get_va_pages(vmi, dtb);
    GSList *loop = pages;
    while(loop) {

        page_info_t *page = loop->data;
        if(page->vaddr < KERNEL_VADDR) {
            // In case the multiple vaddr points to the same page, we might already have an event here
            vmi_event_t *new_event = vmi_get_mem_event(vmi, page->paddr, VMI_MEMEVENT_PAGE);
            if (!new_event) {
                new_event = g_malloc0(sizeof(vmi_event_t));

                // TODO 1: Setup a mem event on this page and register it

                // We are saving the event into this list so we can easily clean up later
                events = g_slist_append(events, new_event);
                printf("Page vaddr: 0x%lx paddr: 0x%lx\n", page->vaddr, page->paddr);
            }
        }

        free(loop->data);
        loop=loop->next;
    }
    g_slist_free(pages);

    vmi_resume_vm(vmi);

    while(!interrupted){
        status = vmi_events_listen(vmi,500);
        if (status != VMI_SUCCESS) {
            printf("Error waiting for events, quitting...\n");
            interrupted = -1;
        }
    }

    vmi_pause_vm(vmi);

    loop = events;
    while(loop) {
        vmi_clear_event(vmi, loop->data, NULL);
        free(loop->data);
        loop = loop->next;
    }
    g_slist_free(events);

    vmi_resume_vm(vmi);

    vmi_destroy(vmi);

    return 0;
}
