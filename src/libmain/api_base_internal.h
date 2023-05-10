/** @file api_base_internal.h
 *
 *  For use only by other api_... files
 */

// Copyright (C) 2015-2023 Sanford Rockowitz <rockowitz@minsoft.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef API_BASE_INTERNAL_H_
#define API_BASE_INTERNAL_H_

#include "config.h"

#include <assert.h>
#include <stdbool.h>
#ifdef ENABLE_SYSLOG
#include <syslog.h>
#endif
#include <threads.h>

#include "public/ddcutil_status_codes.h"
#include "public/ddcutil_c_api.h"

#include "base/per_thread_data.h"

#define DDCA_PRECOND_STDERR 0x01
#define DDCA_PRECOND_RETURN 0x02

typedef enum {
   DDCA_PRECOND_STDERR_ABORT  = DDCA_PRECOND_STDERR,
   DDCA_PRECOND_STDERR_RETURN = DDCA_PRECOND_STDERR | DDCA_PRECOND_RETURN,
   DDCA_PRECOND_RETURN_ONLY   = DDCA_PRECOND_RETURN
} DDCA_Api_Precondition_Failure_Mode;


extern bool library_initialized;

extern DDCA_Api_Precondition_Failure_Mode api_failure_mode;

#define API_PRECOND(expr) \
   do { \
      if (!(expr)) { \
         SYSLOG(LOG_ERR, "Precondition failed: \"%s\" in file %s at line %d",  \
                         #expr, __FILE__,  __LINE__);   \
         if (api_failure_mode & DDCA_PRECOND_STDERR) {  \
            DBGTRC_NOPREFIX(true, DDCA_TRC_ALL, "Precondition failure (%s) in function %s at line %d of file %s", \
                         #expr, __func__, __LINE__, __FILE__); \
            fprintf(stderr, "Precondition failure (%s) in function %s at line %d of file %s\n", \
                             #expr, __func__, __LINE__, __FILE__); \
         } \
         if (api_failure_mode & DDCA_PRECOND_RETURN)  \
            return DDCRC_ARG;  \
         /* __assert_fail(#expr, __FILE__, __LINE__, __func__); */ \
         abort();  \
      } \
   } while (0)

#define API_PRECOND_W_EPILOG(expr) \
   do { \
      if (!(expr)) { \
         SYSLOG(LOG_ERR, "Precondition failed: \"%s\" in file %s at line %d",  \
                         #expr, __FILE__,  __LINE__);   \
         if (api_failure_mode & DDCA_PRECOND_STDERR) {  \
            DBGTRC_NOPREFIX(true, DDCA_TRC_ALL, "Precondition failure (%s) in function %s at line %d of file %s", \
                         #expr, __func__, __LINE__, __FILE__); \
            fprintf(stderr, "Precondition failure (%s) in function %s at line %d of file %s\n", \
                             #expr, __func__, __LINE__, __FILE__); \
         } \
         if (!(api_failure_mode & DDCA_PRECOND_RETURN))  \
            abort();  \
         trace_api_call_depth--; \
         DBGTRC_RET_DDCRC(true, DDCA_TRC_ALL, DDCRC_ARG, "Precondition failure: %s=NULL", (expr)); \
         return DDCRC_ARG;  \
      } \
   } while (0)


#define API_PRECOND_RVALUE(expr) \
   ( { DDCA_Status ddcrc = 0; \
       if (!(expr)) { \
          SYSLOG(LOG_ERR, "Precondition failed: \"%s\" in file %s at line %d",  \
                          #expr, __FILE__,  __LINE__);   \
          if (api_failure_mode & DDCA_PRECOND_STDERR) {  \
             DBGTRC_NOPREFIX(true, DDCA_TRC_ALL, "Precondition failure (%s) in function %s at line %d of file %s", \
                          #expr, __func__, __LINE__, __FILE__); \
             fprintf(stderr, "Precondition failure (%s) in function %s at line %d of file %s\n", \
                             #expr, __func__, __LINE__, __FILE__); \
          } \
          if (!(api_failure_mode & DDCA_PRECOND_RETURN))  \
             abort(); \
          ddcrc = DDCRC_ARG;  \
       } \
       ddcrc; \
    } )

#ifdef UNUSED
#define API_PRECOND_NORC(expr) \
   do { \
      if (!(expr)) { \
         SYSLOG(LOG_ERR, "Precondition failed: \"%s\" in file %s at line %d",  \
                            #expr, __FILE__,  __LINE__);   \
         if (api_failure_mode & DDCA_PRECOND_STDERR) \
             fprintf(stderr, "Precondition failure (%s) in function %s at line %d of file %s\n", \
                             #expr, __func__, __LINE__, __FILE__); \
         if (api_failure_mode & DDCA_PRECOND_RETURN)  \
            return;  \
         abort(); \
      } \
   } while (0)
#endif


//
// Precondition Failure Mode
//

DDCA_Api_Precondition_Failure_Mode
ddca_set_precondition_failure_mode(
      DDCA_Api_Precondition_Failure_Mode failure_mode);

DDCA_Api_Precondition_Failure_Mode
ddca_get_precondition_failure_mode();



#ifdef UNUSED
#define ENSURE_LIBRARY_INITIALIZED() \
      do { \
         if (!library_initialized)  { \
            ddca_init(DDCA_INIT_OPTIONS_DISABLE_CONFIG_FILE); \
         } \
      } while (0)
#endif

#define API_PROLOG(debug_flag, format, ...) \
   do { \
      if (!library_initialized)  { \
         ddca_init(NULL, DDCA_INIT_OPTIONS_DISABLE_CONFIG_FILE); \
      } \
      if (trace_api_call_depth > 0 || is_traced_api_call(__func__) ) \
         trace_api_call_depth++; \
      dbgtrc( (debug_flag) ? DDCA_TRC_ALL : DDCA_TRC_API, DBGTRC_OPTIONS_NONE, \
            __func__, __LINE__, __FILE__, "Starting  "format, ##__VA_ARGS__); \
      if (ptd_api_profiling_enabled) ptd_profile_function_start(__func__); \
  } while(0)

#ifdef UNUSED
#define API_PROLOGX(debug_flag, _trace_groups, format, ...) \
   do { \
      if (!library_initialized)  { \
         ddca_init(DDCA_INIT_OPTIONS_DISABLE_CONFIG_FILE); \
      } \
      trace_api_call_depth++; \
      dbgtrc( (debug_flag) ? DDCA_TRC_ALL : (_trace_groups), DBGTRC_OPTIONS_NONE, \
            __func__, __LINE__, __FILE__, "Starting  "format, ##__VA_ARGS__); \
  } while(0)
#endif


#define API_EPILOG(_debug_flag, _rc, _format, ...) \
   do { \
        dbgtrc_ret_ddcrc( \
          (_debug_flag) ? DDCA_TRC_ALL : DDCA_TRC_API, DBGTRC_OPTIONS_NONE, \
          __func__, __LINE__, __FILE__, _rc, _format, ##__VA_ARGS__); \
        if (trace_api_call_depth > 0) \
           trace_api_call_depth--; \
        if (ptd_api_profiling_enabled) ptd_profile_function_end(__func__); \
        return _rc; \
   } while(0)


#define API_EPILOG_WO_RETURN(_debug_flag, _rc, _format, ...) \
   do { \
        dbgtrc_ret_ddcrc( \
          (_debug_flag) ? DDCA_TRC_ALL : DDCA_TRC_API, DBGTRC_OPTIONS_NONE, \
          __func__, __LINE__, __FILE__, _rc, _format, ##__VA_ARGS__); \
        if (trace_api_call_depth > 0) \
           trace_api_call_depth--; \
        if (ptd_api_profiling_enabled) ptd_profile_function_end(__func__); \
   } while(0)

#ifdef UNUSED
#define API_EPILOGX(_debug_flag, _trace_groups, _rc, _format, ...) \
   do { \
        dbgtrc_ret_ddcrc( \
          (_debug_flag) ? DDCA_TRC_ALL : _trace_groups, DBGTRC_OPTIONS_NONE, \
          __func__, __LINE__, __FILE__, _rc, _format, ##__VA_ARGS__); \
        trace_api_call_depth--; \
        return rc; \
   } while(0)
#endif


#ifdef UNUSED
#define ENABLE_API_CALL_TRACING() \
   do { if (is_traced_api_call(__func__)) {\
         tracing_cur_api_call = true; \
         trace_api_call_depth++; \
         /* printf("(%s) Setting tracing_cur_api_call = %s\n", __func__,  sbool(tracing_cur_api_call) ); */ \
      } \
   } while(0)
#endif

#ifdef OLD
#define DISABLE_API_CALL_TRACING() \
   do { \
      if (tracing_cur_api_call) { \
          printf("(%s) Setting tracing_cur_api_call = false\n", __func__);  \
          tracing_cur_api_call = false; \
      } \
      trace_api_call_depth--; \
   } while(0)
#endif

#define DISABLE_API_CALL_TRACING() \
   do { \
      if (trace_api_call_depth > 0) \
         trace_api_call_depth--; \
   } while(0)



//
// Unpublished
//

#ifdef UNNEEDED
/** Turn on call stack tracing for a specific API function.
 *
 *  @param[in]  funcname   function name
 *  @return true if function is traceable, false if not
 *
 *  @remark
 *  The function must include trace calls.
 */
bool
ddca_add_traced_api_call(
      const char * funcname);
#endif


#ifdef REMOVED
   /***
   I2C is an inherently unreliable protocol.  The application is responsible for
   retry management.
   The maximum number of retries can be tuned.
   There are 3 retry contexts:
   - An I2C write followed by a read.  Most DDC operations are of this form.
   - An I2C write without a subsequent read.  DDC operations to set a VCP feature
     value are in this category.
   - Some DDC operations, such as reading the capabilities string, reading table
     feature and writing table features require multiple write/read exchanges.
     These multi-part exchanges have a separate retry count for the entire operation.
   */
   ///@{
   /** Gets the upper limit on a max tries value that can be set.
    *
    * @return maximum max tries value allowed on set_max_tries()
    */
   int
   ddca_max_max_tries(void);

   /** Gets the maximum number of I2C tries for the specified operation type.
    * @param[in]  retry_type   I2C operation type
    * @return maximum number of tries
    *
    * @remark
    * This setting is global, not thread-specific.
    */
   int
   ddca_get_max_tries(
       DDCA_Retry_Type retry_type);

   /** Sets the maximum number of I2C retries for the specified operation type
    * @param[in] retry_type    I2C operation type
    * @param[in] max_tries     maximum count to set
    * @retval    DDCRC_ARG     max_tries < 1 or > #ddca_get_max_tries()
    *
    * @remark
    * This setting is global, not thread-specific.
    */
   DDCA_Status
   ddca_set_max_tries(
       DDCA_Retry_Type retry_type,
       int             max_tries);
   ///@}
#endif


#ifdef REMOVED

   //
   // Performance Options
   //


   /** Sets the sleep multiplier factor to be used for newly detected displays.
    *
    *  @param[in]  multiplier, must be >= 0 and <= 10
    *  @return     old multiplier, -1.0f if invalid multiplier specified
    *
    *  The semantics of this function has changed.  Prior to release 1.5,
    *  this function set the sleep multiplier for new threads.
    *  As of release 1.5, the sleep multiplier is maintained per
    *  display, not per thread.  This function sets the sleep multiplier
    *  for newly detected displays.
    *  @remark
    *  This function is intended for use only during program initialization,
    *  typically from a value passed on the command line.
    *  Consequently there are no associated lock/unlock functions for the value.
    */
   double
   ddca_set_default_sleep_multiplier(double multiplier);

   /** Gets the sleep multiplier factor used for newly detected displays.
    *
    *  The semantics of this function has changed.
    *  See #ddca_set_default_sleep_multiplier().
    *
    * @return multiplier
    */
   double
   ddca_get_default_sleep_multiplier();

   /** @deprecated use #ddca_set_default_sleep_multiplier()
    */
   __attribute__ ((deprecated ("use ddca_set_default_sleep_multiplier")))
   void
   ddca_set_global_sleep_multiplier(double multiplier);

   /** @deprecated use #ddca_get_default_sleep_multiplier()
    */
   __attribute__ ((deprecated ("use ddca_get_default_sleep_multiplier")))
   double
   ddca_get_global_sleep_multiplier();


#endif

#ifdef RESERVED
   // Deprecated, has no effect
   __attribute__ ((deprecated ("has no effect")))
   bool
   ddca_enable_sleep_suppression(bool newval); ;

   __attribute__ ((deprecated ("always returns false")))
   bool
   ddca_is_sleep_suppression_enabled();
#endif

#ifdef RESERVED
   //
   // Tracing
   //

   /** Turn on tracing for a specific function.
    *
    *  @param[in]  funcname   function name
    *  @return true if function is traceable, false if not
    *
    *  @remark
    *
    *  The function must include trace calls.
    */
   bool
   ddca_add_traced_function(
         const char * funcname);

   /** Turn on all tracing in a specific source file.
    *
    *  @param[in] filename  simple file name, with or without the ".c" extension,
    *                        e.g. vcp_feature_set.c, vcp_feature_set
    */
   void
   ddca_add_traced_file(
         const char * filename);

   /** Replaces the groups being traced
    *
    *  @param[in] trace_flags  bitfield indicating groups to trace
    */
   void
   ddca_set_trace_groups(
         DDCA_Trace_Group trace_flags);

   /** Adds to the groups being traced
    *
    *  @param[in] trace_flags  bitfield indicating groups to trace
    *
    *  @since 1.2.0
    */
   void
   ddca_add_trace_groups(
         DDCA_Trace_Group trace_flags);


   /** Given a trace group name, returns its identifier.
    *  Case is ignored.
    *
    *  @param[in] name trace group name
    *  @return    trace group identifier
    *  @retval    TRC_NEVER unrecognized name
    */
   DDCA_Trace_Group
   ddca_trace_group_name_to_value(char * name);

   /** Sets tracing options
    *
    *  @param[in] options  enum that can be used as bit flags
    */
   void
   ddca_set_trace_options(DDCA_Trace_Options  options);

#endif

#ifdef REMOVED
typedef enum {
   DDCA_TRCOPT_TIMESTAMP = 0x01,
   DDCA_TRCOPT_THREAD_ID = 0x02,
   DDCA_TRCOPT_WALLTIME  = 0x04
} DDCA_Trace_Options;
#endif



#endif /* API_BASE_INTERNAL_H_ */
