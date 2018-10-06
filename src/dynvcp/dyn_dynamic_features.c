/** @file ddc_dynamic_features.c
 *
 *  Maintain user-defined (aka dynamic) feature definitions
 */

// Copyright (C) 2018 Sanford Rockowitz <rockowitz@minsoft.com>
// SPDX-License-Identifier: GPL-2.0-or-later

/** \cond */
#include <ctype.h>

#include <assert.h>
#include <stddef.h>
#include <glib-2.0/glib.h>
#include <string.h>
#include <wordexp.h>
#include <unistd.h>

#include "ddcutil_types.h"
#include "ddcutil_status_codes.h"

#include "util/error_info.h"
#include "util/edid.h"
#include "util/glib_util.h"
#include "util/string_util.h"
#include "util/file_util.h"
/** \endcond */

#include "base/core.h"
#include "base/displays.h"
#include "base/dynamic_features.h"
#include "base/monitor_model_key.h"

#include "dyn_dynamic_features.h"


// Default trace class for this file
static DDCA_Trace_Group TRACE_GROUP = DDCA_TRC_UDF;

/* static */ bool enable_dynamic_features = true;


// for now, just use an array of pointers to DDCA_Feature_Metadata

// static DDCA_Feature_Metadata * mfg_feature_info[32];


// array of features needs to be per-monitor

// for each detected monitor, look for files with name of form:
//   MFG_modelName_productCode.MCCS
//   spaces and other special characters converted to "_" in modelName

// search path:
//   local directory
//   XDG search path, use directory .local/share/ddcutil

// once we have a dref, can look for feature definition files

// functions:
//   find_feature_definition_file(char * mfg, char * model, uint16_t product_code)
//   load_feature_definition_file(char * fn)
//        creates an array of DDCA_Feature_Metadata
//   hang it off dref
//      dref needs additional flag DREF_LOCAL_FEATURES_SEARCHED
//
// vcp feature code lookup:
//   need function to add local tables
//      create VCP_Feature_Table_Entry from DDCA_Feature_Metadata
//   modify find_feature_by_hexid() to use local tables
//     problem: need to specify which monitor-specific list of local tables to use
//         function variant that takes aux table as argument?
//         perhaps put this function at ddc level?
//
//



static GHashTable * dynamic_features_records;

// entries are only added, never deleted or replaced
void
dfr_init() {
   if (!dynamic_features_records) {
      dynamic_features_records = g_hash_table_new(
                                   g_str_hash,
                                   g_str_equal);
   }
}

void dfr_save(
      Dynamic_Features_Rec * dfr)
{
   char * key = model_id_string(
                   dfr->mfg_id,
                   dfr->model_name,
                   dfr->product_code);
   if (!dynamic_features_records)
      dfr_init();
   g_hash_table_insert(dynamic_features_records, key, dfr);
}

Dynamic_Features_Rec *
dfr_lookup(
      char *   mfg_id,
      char *   model_name,
      uint16_t product_code)
{
   Dynamic_Features_Rec * result = NULL;
   if (dynamic_features_records) {
      char * key = model_id_string(mfg_id, model_name, product_code);
      result = g_hash_table_lookup(dynamic_features_records, key);
      assert(memcmp(result->marker, DYNAMIC_FEATURES_REC_MARKER, 4) == 0);
      // if (result->flags & DFR_FLAGS_NOT_FOUND)
      //    result = NULL;
      free(key);
   }
   return result;
}

Dynamic_Features_Rec *
dfr_get(
      char *   mfg_id,
      char *   model_name,
      uint16_t product_code)
{
   Dynamic_Features_Rec * result = NULL;
   Dynamic_Features_Rec * existing = NULL;

   existing = dfr_lookup(mfg_id, model_name, product_code);
   if (existing) {
      if (existing->flags & DFR_FLAGS_NOT_FOUND) {
         // we've already checked and it's not there
         result = NULL;
      }
      else
         result = existing;
   }
   else {
      // ??? what to do with errors from loading file?
   }
   return result;
}


/* static */ DDCA_Feature_Metadata *
get_feature_metadata(
      GHashTable * features,
      uint8_t      feature_code)
{
   DDCA_Feature_Metadata * result =
   g_hash_table_lookup(features, GINT_TO_POINTER(feature_code));
   if (result)
      assert( memcmp(result->marker, DDCA_FEATURE_METADATA_MARKER, 4) == 0);
   return result;
}


/** Look for feature definition file in the current directory and
 *  on the XDG search path.
 *
 *  \param simple_fn  simple filename, without ".mccs" suffix
 *  \return fully qualified name of file found, NULL if not found (caller must free)
 *
 *  \remark
 *  Consider generalizing, moving to file_util.c
 */
static
char *
find_feature_def_file(
      const char * simple_fn)
{
   bool debug = false;
   DBGTRC(debug, TRACE_GROUP, "Starting.  simple_fn=|%s|", simple_fn);
   char * result = NULL;

   char * paths[] = {
         ".",      // current directory
         "~/.local/share/ddcutil",
         "/usr/local/share/ddcutil",
         "/usr/share/ddcutil"
   };
   int paths_ct = ARRAY_SIZE(paths);

   for (int ndx = 0; ndx < paths_ct; ndx++ ) {
      wordexp_t exp_result;
      int wordexp_flags = 0;
      wordexp_flags = WRDE_SHOWERR;           // TEMP
      wordexp(paths[ndx], &exp_result, wordexp_flags);
      char * epath = exp_result.we_wordv[0];
      char fqnamebuf[PATH_MAX];
      snprintf(fqnamebuf, PATH_MAX, "%s/%s.mccs", epath, simple_fn);
      // DBGMSF(debug, "fqnamebuf:  |%s|", fqnamebuf);
      wordfree(&exp_result);
      if (access(fqnamebuf, R_OK) == 0) {
         result = strdup(fqnamebuf);
         break;
      }
   }

   DBGTRC(debug, TRACE_GROUP, "Returning: |%s|", result);
   return result;
}

#ifdef OLD
// refactored to file_getlines_errinfo()
// reads a feature definition file into an array of text lines
Error_Info *
read_feature_definition_file(
      const char *  filename,
      GPtrArray *   lines)
{
   Error_Info * errs = NULL;

   int rc = file_getlines(filename,  lines, false);
   if (rc < 0) {
      char * detail = g_strdup_printf("Error reading file %s", filename);
      errs = errinfo_new2(
            rc,
            __func__,
            detail);
      // TODO: variant of errinfo_new2() that puts detail last, as variable args (detail_fmt, ...)
      free(detail);
   }
   return errs;
}
#endif


Error_Info *
dfr_load_by_edid(
      Parsed_Edid *           edid,
      Dynamic_Features_Rec ** dfr_loc)
{
   bool debug = false;

   Error_Info *           errs = NULL;
   Dynamic_Features_Rec * dfr  = NULL;
   char * simple_fn = model_id_string(edid->mfg_id, edid->model_name,edid->product_code);

   char * fqfn = find_feature_def_file(simple_fn);
   if (fqfn) {
      // read file into GPtrArray * lines
      GPtrArray * lines = g_ptr_array_new();
      // errs = read_feature_definition_file(fqfn, lines);
      errs = file_getlines_errinfo(fqfn, lines);

      if (!errs) {
         errs = create_monitor_dynamic_features(
             edid->mfg_id,
             edid->model_name,
             edid->product_code,
             lines,
             fqfn,
             &dfr);
         // TODO: check that dfr == NULL if error
         assert( (errs && !dfr) || (!errs && dfr));
      }
      free(fqfn);
   }
   else {
      // DBGMSG("simple=fn=%s", simple_fn);
      errs = errinfo_new2(DDCRC_NOT_FOUND, __func__,
                          "Feature definition file not found: %s.mccs", simple_fn);
   }
   assert( (errs && !dfr) || (!errs && dfr));   // avoid clang warning

   if (errs) {
      dfr = dfr_new(edid->mfg_id, edid->model_name, edid->product_code, NULL);
      dfr->flags |= DFR_FLAGS_NOT_FOUND;
   }
   else {
      *dfr_loc = dfr;
   }

   free(simple_fn);
   dfr_save(dfr);

   if (debug || IS_TRACING()) {
      if (errs) {
         DBGMSG("Done.  Returning errs: ");
         errinfo_report(errs, 1);
      }
      else
         DBGMSG("Done.  *dfr_loc=%p", *dfr_loc);
   }

   return errs;
}


Error_Info *  dfr_check_by_dref(Display_Ref * dref) {
   bool debug = false;
   DBGTRC(debug, TRACE_GROUP, "Starting. ");

   Error_Info * errs = NULL;
   if (!enable_dynamic_features)    // global variable
      goto bye;

   if ( !(dref->flags & DREF_DYNAMIC_FEATURES_CHECKED) ) {
      // DBGMSF(debug, "DREF_DYNAMIC_FEATURES_CHECKED not yet set");
      dref->dfr = NULL;

      Dynamic_Features_Rec * dfr = NULL;
      errs = dfr_load_by_edid(dref->pedid, &dfr);
      if (!errs) {
         dref->dfr = dfr;
      }

      dref->flags |= DREF_DYNAMIC_FEATURES_CHECKED;
   }

bye:
   if (debug || IS_TRACING_GROUP(DDCA_TRC_UDF) ) {
      if (errs) {
         DBGMSG("Done.  Returning errs: ");
         errinfo_report(errs, 1);
      }
      else
         DBGMSG("Done.  dref->dfr=%p", dref->dfr);
   }
   return errs;
}


