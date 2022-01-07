/** \f ore_per_thread_settings.c */

// Copyright (C) 2021 Sanford Rockowitz <rockowitz@minsoft.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#define GNU_SOURCE    // for syscall()

#include <glib-2.0/glib.h>
#include <stdio.h>
#include <stdbool.h>

#ifdef TARGET_BSD
#include <pthread_np.h>
#else
#include <sys/types.h>
#include <sys/syscall.h>
#include <syslog.h>
#endif
#include <unistd.h>

#include "util/report_util.h"

#include "ddcutil_types.h"
 
#include "core_per_thread_settings.h"


//
// Global SDTOUT and STDERR redirection, for controlling message output in API
//

/** @defgroup output_redirection Basic Output Redirection
 */

#ifdef OVERKILL
#define FOUT_STACK_SIZE 8

static FILE* fout_stack[FOUT_STACK_SIZE];
static int   fout_stack_pos = -1;
#endif

// controls access to default_thread_output_settings
static GMutex default_thread_output_settings_mutex;

static Thread_Output_Settings * default_thread_output_settings = NULL;


static void allocate_default_thread_output_settings() {
   default_thread_output_settings = g_new0(Thread_Output_Settings, 1);
   default_thread_output_settings->fout = stdout;
   default_thread_output_settings->ferr = stderr;
   default_thread_output_settings->output_level = DDCA_OL_NORMAL;
}


/** Gets all settings to be used for new threads */
Thread_Output_Settings * get_default_thread_output_settings() {
   g_mutex_lock(&default_thread_output_settings_mutex);
   if ( !default_thread_output_settings )
      allocate_default_thread_output_settings();
   // return a copy so that struct is in a consistent state when used by the caller
   Thread_Output_Settings * result =  g_new0(Thread_Output_Settings, 1);
   memcpy(result, default_thread_output_settings, sizeof(Thread_Output_Settings));
   g_mutex_unlock(&default_thread_output_settings_mutex);
   return result;
}


/** Sets the fout and ferr values to be used for newly created threads */
void set_default_thread_output_settings(FILE * fout, FILE * ferr) {
   bool debug = false;
   if (debug)
      printf("(%s) fout=%p, ferr=%p, stdout=%p, stderr=%p\n",
             __func__, fout, ferr, stdout, stderr);
   g_mutex_lock(&default_thread_output_settings_mutex);
   if ( !default_thread_output_settings )
      allocate_default_thread_output_settings();
   if (fout)
      default_thread_output_settings->fout = fout;
   if (ferr)
      default_thread_output_settings->ferr = ferr;
   g_mutex_unlock(&default_thread_output_settings_mutex);
}


/** Sets the output_level to be used for newly created threads */
void set_default_thread_output_level(DDCA_Output_Level ol) {
   bool debug = false;
   if (debug)
      printf("(%s) ol=%s\n", __func__, output_level_name(ol));
   g_mutex_lock(&default_thread_output_settings_mutex);
   if ( !default_thread_output_settings )
      allocate_default_thread_output_settings();
   default_thread_output_settings->output_level = ol;
   g_mutex_unlock(&default_thread_output_settings_mutex);
}


/** Gets Thread_Output_Settings struct for the current thread */
Thread_Output_Settings *  get_thread_settings() {
   static GPrivate per_thread_dests_key = G_PRIVATE_INIT(g_free);
   bool debug = false;

   Thread_Output_Settings *settings = g_private_get(&per_thread_dests_key);

   // GThread * this_thread = g_thread_self();
   // printf("(%s) this_thread=%p, settings=%p\n", __func__, this_thread, settings);

   if (!settings) {
      settings = get_default_thread_output_settings();
      settings->tid = get_thread_id();
      g_private_set(&per_thread_dests_key, settings);
      if (debug)
         printf("(%s) Allocated settings=%p for thread %ld, fout=%p, ferr=%p, stdout=%p, stderr=%p\n",
               __func__, settings, settings->tid, settings->fout, settings->ferr, stdout, stderr);
   }

   // printf("(%s) Returning: %p\n", __func__, settings);
   return settings;
}


// Issue: How to specify that output should be discarded vs reset to stdout?
// issue will resetting report dest cause conflicts?
// To reset to STDOUT, use constant stdout in stdio.h  - NO - screws up rpt_util
// problem:

/** Redirect output on the current thread that would normally go to **stdout**.
 *
 *  @param fout pointer to output stream
 *
 * @ingroup output_redirection
 */
void set_fout(FILE * fout) {
   bool debug = false;

   Thread_Output_Settings * dests = get_thread_settings();
   dests->fout = fout;
   if (debug)
      printf("(%s) tid=%ld, dests=%p, fout=%p, stdout=%p\n",
             __func__, dests->tid, dests, fout, stdout);
   // FOUT = fout;
   rpt_change_output_dest(fout);
}


/** Redirect output that would normally go to **stdout** back to **stdout**.
 * @ingroup output_redirection
 */
void set_fout_to_default() {
   // FOUT = stdout;
   Thread_Output_Settings * default_settings = get_default_thread_output_settings();
   Thread_Output_Settings * dests = get_thread_settings();
   dests->fout =  default_settings->fout;
   free(default_settings);
   rpt_change_output_dest(dests->fout);
}


/** Redirect output that would normally go to **stderr**..
 *
 *  @param ferr pointer to output stream
 *
 *  @ingroup output_redirection
 */
void set_ferr(FILE * ferr) {
   Thread_Output_Settings * dests = get_thread_settings();
   dests->ferr = ferr;
}


/** Redirect output that would normally go to **stderr** back to **stderr**.
 * @ingroup output_redirection
 */
void set_ferr_to_default() {
   Thread_Output_Settings * default_settings = get_default_thread_output_settings();
   Thread_Output_Settings * dests = get_thread_settings();
   dests->ferr =  default_settings->ferr;
   free(default_settings);
}


/** Gets the "stdout" destination for the current thread
 *
 *  @return output destination
 *
 *  @ingroup output_redirection
 */
FILE * fout() {
   Thread_Output_Settings * dests = get_thread_settings();
   // printf("(%s) tid=%ld, dests=%p, dests->fout=%p, stdout=%p\n",
   //        __func__, get_thread_id(), dests, dests->fout, stdout);
   return dests->fout;
}


/** Gets the "stderr" destination for the current thread
 *
 *  @return output destination
 *
 *  @ingroup output_redirection
 */
FILE * ferr() {
   Thread_Output_Settings * dests = get_thread_settings();
   return dests->ferr;
}


#ifdef OVERKILL

// Functions that allow for temporarily changing the output destination.


void push_fout(FILE* new_dest) {
   assert(fout_stack_pos < FOUT_STACK_SIZE-1);
   fout_stack[++fout_stack_pos] = new_dest;
}


void pop_fout() {
   if (fout_stack_pos >= 0)
      fout_stack_pos--;
}


void reset_fout_stack() {
   fout_stack_pos = 0;
}


FILE * cur_fout() {
   // special handling for unpushed case because can't statically initialize
   // output_dest_stack[0] to stdout
   return (fout_stack_pos < 0) ? stdout : fout_stack[fout_stack_pos];
}
#endif


//
// Message level control for normal output
//

/** \defgroup msglevel Message Level Management
 *
 * Functions and variables to manage and query output level settings.
 */

/** Returns the output level for the current thread
 *
 * @return output level
 *
 * \ingroup msglevel
 */
DDCA_Output_Level get_output_level() {
   Thread_Output_Settings * settings = get_thread_settings();
   return settings->output_level;
}


/** Sets the output level for the current thread
 *
 * @param newval output level to set
 * @return old output level
 *
 *  \ingroup msglevel
 */
DDCA_Output_Level set_output_level(DDCA_Output_Level newval) {
   Thread_Output_Settings * settings = get_thread_settings();
   DDCA_Output_Level old_level = settings->output_level;
   settings->output_level = newval;
   return old_level;
}


/** Gets the printable name of an output level.
 *
 * @param val  output level
 * @return printable name for output level
 *
 *  \ingroup msglevel
 */
// const  adding "const" causes api change ddca_output_level_name(), defer until next api change
char * output_level_name(DDCA_Output_Level val) {
   char * result = NULL;
   switch (val) {
      case DDCA_OL_TERSE:
         result = "Terse";
         break;
      case DDCA_OL_NORMAL:
         result = "Normal";
         break;
      case DDCA_OL_VERBOSE:
         result = "Verbose";
         break;
      case DDCA_OL_VV:
         result = "Very Vebose";
      // default unnecessary, case exhausts enum
   }
   return result;
}


// get_thread_id() and get_process_id() probably should be in a util level file

/** Gets the id number of the current thread
 *
 *  @ return  thread number
 */
intmax_t get_thread_id() {
   bool debug = false;
   if (debug)
      printf("(%s) Starting.\n", __func__);
#ifdef TARGET_BSD
   int tid = pthread_getthreadid_np();
#else
   pid_t tid = syscall(SYS_gettid);
#endif
   if (debug)
      printf("(%s) Done.    Returning %ld\n", __func__, (intmax_t) tid);
   return tid;
}


/** Gets the id number of the current process
 *
 *  @ return  process number
 */
intmax_t get_process_id()
{
   pid_t pid = syscall(SYS_getpid);
   return pid;
}

