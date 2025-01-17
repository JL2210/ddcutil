/** @file ddc_watch_displays.c
 *
 *  Watch for monitor addition and removal
 */

// Copyright (C) 2021-2024 Sanford Rockowitz <rockowitz@minsoft.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#define _GNU_SOURCE    // for usleep()

#include "config.h"
#include "public/ddcutil_types.h"

/** \cond */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <glib-2.0/glib.h>
#ifdef ENABLE_UDEV
#include <libudev.h>
#endif
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util/coredefs.h"
#include "util/data_structures.h"
#include "util/debug_util.h"
#include "util/drm_common.h"
#include "util/file_util.h"
#include "util/glib_string_util.h"
#include "util/glib_util.h"
#include "util/i2c_util.h"
#include "util/linux_util.h"
#include "util/report_util.h"
#include "util/string_util.h"
#include "util/subprocess_util.h"
#include "util/sysfs_util.h"
#include "util/udev_util.h"

#include "base/core.h"
#include "base/displays.h"
#include "base/ddc_errno.h"
#include "base/drm_connector_state.h"
#include "base/i2c_bus_base.h"
#include "base/linux_errno.h"
#include "base/rtti.h"
/** \endcond */

#include "i2c/i2c_sysfs_base.h"
#include "i2c/i2c_bus_core.h"
#include "i2c/i2c_dpms.h"

#include "ddc/ddc_displays.h"
#include "ddc/ddc_packet_io.h"
#include "ddc/ddc_status_events.h"
#include "ddc/ddc_vcp.h"

#include "ddc/ddc_watch_displays_common.h"
#include "ddc/ddc_watch_displays.h"


// Trace class for this file
static DDCA_Trace_Group TRACE_GROUP = DDCA_TRC_NONE;

bool             ddc_slow_watch = false;
int              extra_stabilization_millisec = DEFAULT_EXTRA_STABILIZATION_MILLISEC;
int              stabilization_poll_millisec  = DEFAULT_STABILIZATION_POLL_MILLISEC;
#ifdef SECONDARY_UDEV_RECEIVE
int              secondary_udev_receive_millisec = DEFAULT_SECONDARY_UDEV_RECEIVE_MILLISEC;
#endif
bool             use_sysfs_connector_id = true;
bool             report_udev_events;
int              udev_poll_loop_millisec = DEFAULT_UDEV_POLL_LOOP_MILLISEC;   // 2000;   // default sleep time on each loop

STATIC
void free_watch_displays_data(Watch_Displays_Data * wdd) {
   if (wdd) {
      assert( memcmp(wdd->marker, WATCH_DISPLAYS_DATA_MARKER, 4) == 0 );
      wdd->marker[3] = 'x';
      free(wdd);
   }
}


void dbgrpt_udev_device(struct udev_device * dev, bool verbose, int depth) {
   rpt_structure_loc("udev_device", dev, depth);
   int d1 = depth+1;
   // printf("   Node: %s\n", udev_device_get_devnode(dev));         // /dev/dri/card0
   // printf("   Subsystem: %s\n", udev_device_get_subsystem(dev));  // drm
   // printf("   Devtype: %s\n", udev_device_get_devtype(dev));      // drm_minor

   rpt_vstring(d1, "Action:      %s", udev_device_get_action(   dev));     // "change"
   rpt_vstring(d1, "devpath:     %s", udev_device_get_devpath(  dev));
   rpt_vstring(d1, "subsystem:   %s", udev_device_get_subsystem(dev));     // drm
   rpt_vstring(d1, "devtype:     %s", udev_device_get_devtype(  dev));     // drm_minor
   rpt_vstring(d1, "syspath:     %s", udev_device_get_syspath(  dev));
   rpt_vstring(d1, "sysname:     %s", udev_device_get_sysname(  dev));
   rpt_vstring(d1, "sysnum:      %s", udev_device_get_sysnum(   dev));
   rpt_vstring(d1, "devnode:     %s", udev_device_get_devnode(  dev));     // /dev/dri/card0
   rpt_vstring(d1, "initialized: %d", udev_device_get_is_initialized(  dev));
   rpt_vstring(d1, "driver:      %s", udev_device_get_driver(  dev));

   if (verbose) {
      struct udev_list_entry * entries = NULL;

#ifdef NOT_USEFUL     // see udevadm -p
      entries = udev_device_get_devlinks_list_entry(dev);
      show_udev_list_entries(entries, "devlinks");

      entries = udev_device_get_tags_list_entry(dev);
      show_udev_list_entries(entries, "tags");
#endif

      entries = udev_device_get_properties_list_entry(dev);
      show_udev_list_entries(entries, "properties");

      entries = udev_device_get_sysattr_list_entry(dev);
      //show_udev_list_entries(entries, "sysattrs");
      show_sysattr_list_entries(dev,entries);
   }
}


#ifdef ENABLE_UDEV

//
// Variant using udev 
//

/** Repeatedly calls i2c_detect_buses0() until the value read
 *  equals the prior value.
 *
 *  @oaram prior                       initial array of I2C_Bus_Info for connected buses
 *  @param some_displays_disconnected  if true, add delay to avoid bogus disconnect/connect sequence
 *  @return stabilized array of of Bus_Info for connected buses
 */
GPtrArray *
ddc_i2c_stabilized_buses(GPtrArray* prior, bool some_displays_disconnected) {
   bool debug = false;
   DBGTRC_STARTING(debug, DDCA_TRC_NONE, "prior =%p, some_displays_disconnected=%s",
         prior, SBOOL(some_displays_disconnected));
   Bit_Set_256 bs_prior =  buses_bitset_from_businfo_array(prior, /* only_connected */ true);
   DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "bs_prior:", BS256_REPR(bs_prior));

   // Special handling for case of apparently disconnected displays.
   // It has been observed that in some cases (Samsung U32H750) a disconnect is followed a
   // few seconds later by a connect. Wait a few seconds to avoid triggering events
   // in this case.
   if (some_displays_disconnected) {
      if (extra_stabilization_millisec > 0) {
         char * s = g_strdup_printf(
               "Delaying %d milliseconds to avoid a false disconnect/connect sequence...", extra_stabilization_millisec);
         DBGTRC(debug, TRACE_GROUP, "%s", s);
         SYSLOG2(DDCA_SYSLOG_NOTICE, "%s", s);
         free(s);
         usleep(extra_stabilization_millisec * 1000);
      }
   }

   int stablect = 0;
   bool stable = false;
   while (!stable) {
      // DBGMSG("SLEEPING");
      usleep(1000*stabilization_poll_millisec);
      GPtrArray* latest = i2c_detect_buses0();
      Bit_Set_256 bs_latest =  buses_bitset_from_businfo_array(latest, /* only_connected */ true);
      if (bs256_eq(bs_latest, bs_prior))
            stable = true;
      i2c_discard_buses0(prior);
      prior = latest;
      stablect++;
   }
   if (stablect > 1) {
      DBGTRC(debug || true, TRACE_GROUP,   "Required %d extra calls to i2c_get_buses0()", stablect+1);
      SYSLOG2(DDCA_SYSLOG_NOTICE, "%s required %d extra calls to i2c_get_buses0()", __func__, stablect-1);
   }

   DBGTRC_RETURNING(debug, DDCA_TRC_NONE, BS256_REPR(bs_prior),"");
   return prior;
}


/** Repeatedly reads the edid attibute from the sysfs drm connector dir
 *  whose name has the specfied value.  The value is repeatedly read
 *  until the current value equals the prior value.
 *
 *  @param drm_connector_name    name of connector to check
 *  @param prior_has_edid        value prior to recent sysfs drm change
 *  @return true if edid attribute has value, false if not
 */
bool
ddc_i2c_stabilized_single_bus_by_connector_name(char * drm_connector_name, bool prior_has_edid) {
   bool debug = false;
   // int debug_depth = (debug) ? 1 : -1;
   DBGTRC_STARTING(debug, DDCA_TRC_NONE, "drm_connector_name=%s, prior_has_edid =%s",
         drm_connector_name, SBOOL(prior_has_edid));
   assert(drm_connector_name);

   // Special handling for case of apparently disconnected displays.
   // It has been observed that in some cases (Samsung U32H750) a disconnect is followed a
   // few seconds later by a connect. Wait a few seconds to avoid triggering events
   // in this case.
   if (prior_has_edid) {
      if (extra_stabilization_millisec > 0) {
         char * s = g_strdup_printf(
               "Delaying %d milliseconds to avoid a false disconnect/connect sequence...", extra_stabilization_millisec);
         DBGTRC(debug, TRACE_GROUP, "%s", s);
         SYSLOG2(DDCA_SYSLOG_NOTICE, "%s", s);
         free(s);
         usleep(extra_stabilization_millisec * 1000);
      }
   }

   int stablect = 0;
   bool stable = false;
   while (!stable) {
      usleep(1000*stabilization_poll_millisec);

      char * s = g_strdup_printf("/sys/class/drm/%s/edid", drm_connector_name);
      // DBGF(debug, "reading: %s", s);
      GByteArray* bytes = read_binary_file(s, 2048, true);
      // DBGF(debug, "bytes read: %d", bytes->len);
      bool cur_has_edid = (bytes && bytes->len > 0);
      g_byte_array_free(bytes, true);
      free(s);

      // when MST hub powered on or off, failure in assemble_sysfs_path2()
      // bool cur_has_edid = rpt_attr_edid(debug_depth, NULL, "/sys/class/drm", drm_connector_name, "edid");
      if (cur_has_edid == prior_has_edid)
         stable = true;
      else
         prior_has_edid = cur_has_edid;
      stablect++;
   }
   if (stablect > 1) {
      SYSLOG2(DDCA_SYSLOG_NOTICE, "%s required %d extra calls to rpt_attr_edid()", __func__, stablect-1);
   }

   DBGTRC_RET_BOOL(debug, DDCA_TRC_NONE, prior_has_edid, "Required %d extra calls to rpt_attr_edid()", stablect-1);
   return prior_has_edid;
}


/** Repeatedly reads the edid attibute from the sysfs drm connector dir
 *  whose connector_id has the specfied value.  The value is repeatedly read
 *  until the current value equals the prior value.
 *
 *  @param connector_id          DRM id number of connector to check
 *  @param prior_has_edid        value prior to recent sysfs drm change
 *  @return true if sysfs connnector dir attribute edid has value, false if not
 */
bool
ddc_i2c_stabilized_bus_by_connector_id(int connector_id, bool prior_has_edid) {
   bool debug = false;
   // int debug_depth = (debug) ? 1 : -1;
   DBGTRC_STARTING(debug, DDCA_TRC_NONE, "connector_id=%d, prior_has_edid =%s",
         connector_id, SBOOL(prior_has_edid));

   char * drm_connector_name =  get_sys_drm_connector_name_by_connector_id(connector_id);
   DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "drm_connector_name = |%s|", drm_connector_name);
   assert(drm_connector_name);

   prior_has_edid = ddc_i2c_stabilized_single_bus_by_connector_name(
                          drm_connector_name, prior_has_edid);

   free(drm_connector_name);

   DBGTRC_RET_BOOL(debug, DDCA_TRC_NONE, prior_has_edid, "");
   return prior_has_edid;
}



/** Compares the set of buses currently asleep with the previous list.
 *  If differences exist, either emit events directly or place them on
 *  the deferred events queue.
 *
 *  @param bs_active_bueses  bit set of all buses having edid
 *  @param bs_sleepy_buses   bit set of buses currently asleep
 *  @param events_queue      if null, emit events directly
 *                           if non-null, put events on the queue
 *  @return updated set of buses currently asleep
 */
Bit_Set_256 ddc_i2c_check_bus_asleep(
      Bit_Set_256  bs_active_buses,
      Bit_Set_256  bs_sleepy_buses,
      GArray*      events_queue) // array of DDCA_Display_Status_Event
{
   bool debug = false;
   // two lines so bs256_to_descimal_t() calls don't clobber private thread specific buffer
   DBGTRC_STARTING(debug, DDCA_TRC_NONE, "bs_active_buses: %s", BS256_REPR(bs_active_buses));
   DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "bs_sleepy_buses: %s", BS256_REPR(bs_sleepy_buses));

// #ifdef TEMP
   // remove from the sleepy_connectors array any connector that is not currently active
   // so that it will not be marked asleep when it becomes active
   // i.e. turn off is asleep if connector no longer has a monitor
   bs_sleepy_buses = bs256_and(bs_sleepy_buses, bs_active_buses);

   if (bs256_count(bs_sleepy_buses) > 0)
      DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE,
         "bs_sleepy_buses after removing inactive buses: %s", BS256_REPR(bs_sleepy_buses));

   Bit_Set_256_Iterator iter = bs256_iter_new(bs_active_buses);
   int busno = bs256_iter_next(iter);
   while (busno >= 0) {
      I2C_Bus_Info * businfo = i2c_find_bus_info_in_gptrarray_by_busno(all_i2c_buses, busno);
      if (!businfo->drm_connector_name) {
         DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "Unable to find connector for bus /dev/i2c-%d", busno);
         SEVEREMSG("Unable to find connector for bus /dev/i2c-%d", busno);
      }
      else {
         bool is_dpms_asleep = dpms_check_drm_asleep_by_connector(businfo->drm_connector_name);
         bool last_checked_dpms_asleep = bs256_contains(bs_sleepy_buses, busno);
         if (is_dpms_asleep != last_checked_dpms_asleep) {
            DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "busno = %d, last_checked_dpms_asleep=%s, is_dpms_asleep=%s",
               busno, sbool (last_checked_dpms_asleep), sbool(is_dpms_asleep));
            Display_Ref * dref = DDC_GET_DREF_BY_BUSNO(busno, /* ignore_invalid */ true);
            DDCA_IO_Path iopath;
            iopath.io_mode = DDCA_IO_I2C;
            iopath.path.i2c_busno = busno;
            DDCA_Display_Status_Event evt =
                  ddc_create_display_status_event(
                           (is_dpms_asleep) ? DDCA_EVENT_DPMS_ASLEEP : DDCA_EVENT_DPMS_AWAKE,
                           businfo->drm_connector_name,
                           dref,
                           iopath);
            DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "Queueing %s", display_status_event_repr_t(evt));
            g_array_append_val(events_queue,evt);

            if (is_dpms_asleep) {
               DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "Adding bus %d to sleepy_connectors", busno);
               bs_sleepy_buses = bs256_insert(bs_sleepy_buses, busno);
            }
            else {
               DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "Removing bus %d from sleepy_connectors", busno);
               bs_sleepy_buses = bs256_remove(bs_sleepy_buses, busno);
            }
         }
      }
      // DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "bottom of loop 2, active_connectors->len = %d, sleepy_connectors->len=%d",
      //      bs256_count(bs_active_buses), bs256_count(*p_bs_sleepy_buses));
      busno = bs256_iter_next(iter);
   }
   DBGTRC_DONE(debug, DDCA_TRC_NONE, "Returning: bs_sleepy_buses: %s",  BS256_REPR(bs_sleepy_buses));
   return bs_sleepy_buses;
}


/** Identifies the current list of buses having an edid and compares the
 *  current list with the previous one.  If differences exist, either emit
 *  events directly or place them on the deferred events queue.
 *
 *  @param bs_prev_buses_w_edid  previous set of buses have edid
 *  @param events_queue          if null, emit events directly
 *                               if non-null, put events on the queue
 *  @return updated set of buses having edid
 */
Bit_Set_256 ddc_i2c_check_bus_changes(
      Bit_Set_256 bs_prev_buses_w_edid,
      GArray *    events_queue)
{
   bool debug = false;
   DBGTRC_STARTING(debug, DDCA_TRC_NONE, "bs_prev_buses_w_edid: %s", BS256_REPR(bs_prev_buses_w_edid));

   GPtrArray * new_buses = i2c_detect_buses0();
   Bit_Set_256 bs_new_buses_w_edid =  buses_bitset_from_businfo_array(new_buses, /* only_connected */ true);
   DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "bs_new_buses_w_edid: %s", BS256_REPR(bs_new_buses_w_edid));

   if (!bs256_eq(bs_prev_buses_w_edid, bs_new_buses_w_edid)) {
      // Detect need for special handling for case of display disconnected.
      Bit_Set_256 bs_removed = bs256_and_not(bs_prev_buses_w_edid,bs_new_buses_w_edid);
      DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "bs_removed: %s", BS256_REPR(bs_removed));
      bool detected_displays_removed_flag = bs256_count(bs_removed);

      if (detected_displays_removed_flag) {
         DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "Calling ddc_i2c_stabilized_buses()");
         GPtrArray * stabilized_buses = ddc_i2c_stabilized_buses(new_buses, detected_displays_removed_flag);
         BS256 bs_stabilized_buses_w_edid = buses_bitset_from_businfo_array(stabilized_buses, /*only_connected*/ true);
         DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "bs_stabilized_buses_w_edid: %s", BS256_REPR(bs_stabilized_buses_w_edid));
         // new_buses = stabilized_buses;  // unused
         bs_new_buses_w_edid = bs_stabilized_buses_w_edid;
      }
   }

   bool hotplug_change_handler_emitted = false;
   bool connected_buses_changed = !bs256_eq( bs_prev_buses_w_edid, bs_new_buses_w_edid);
   DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "connected_buses_changed = %s", SBOOL(connected_buses_changed));

   if (connected_buses_changed) {
      BS256 bs_buses_w_edid_removed = bs256_and_not(bs_prev_buses_w_edid, bs_new_buses_w_edid);
      DBGTRC_NOPREFIX(debug, TRACE_GROUP, "bs_buses_w_edid_removed: %s", BS256_REPR(bs_buses_w_edid_removed));

      BS256 bs_buses_w_edid_added = bs256_and_not(bs_new_buses_w_edid, bs_prev_buses_w_edid);
      DBGTRC_NOPREFIX(debug, TRACE_GROUP, "bs_buses_w_edid_added: %s", BS256_REPR(bs_buses_w_edid_added));

      hotplug_change_handler_emitted = ddc_i2c_hotplug_change_handler(
                                           bs_buses_w_edid_removed,
                                           bs_buses_w_edid_added,
                                           events_queue);
   }

   if (hotplug_change_handler_emitted)
      DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "hotplug_change_handler_emitted = %s",
            sbool (hotplug_change_handler_emitted));

   DBGTRC_DONE(debug, TRACE_GROUP, "Returning Bit_Set_256: %s", BS256_REPR(bs_new_buses_w_edid));
   return bs_new_buses_w_edid;
}


/** Searches through all_i2c_buses array of Bus_Info record
 *  to find one with given connector name.
 *
 *  @param  connector_name    DRM connector name
 *  #return bus number from Bus_Info record found, -1 if not found
 */
int search_all_businfo_record_by_connector_name(char *connector_name) {
   bool debug = false;
   DBGTRC_STARTING(debug, DDCA_TRC_NONE, "connector_name = |%s|", connector_name);

   // reads connector dir directly, i.e. does not retrieve persistent data structure
   //  Sys_Drm_Connector * conn = get_drm_connector(connector_name, debug_depth);
   // int busno = conn->i2c_busno;
   // free(conn);
   Connector_Bus_Numbers *cbn = calloc(1, sizeof(Connector_Bus_Numbers));
   get_connector_bus_numbers("/sys/class/drm", connector_name, cbn);
   int busno = cbn->i2c_busno;
   free_connector_bus_numbers(cbn);
   if (busno < 0) {
      DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "Examining businfo records...");
      // look through all businfo records for one with the connector name
      for (int ndx = 0; ndx < all_i2c_buses->len; ndx++) {
         I2C_Bus_Info *businfo = g_ptr_array_index(all_i2c_buses, ndx);
         DBGMSG("Examining businfo record for bus %d, I2C_BUS_PROBED=%s, connector_found_by=%s",
               businfo->busno, sbool(businfo->flags & I2C_BUS_PROBED),
               drm_connector_found_by_name(businfo->drm_connector_found_by));
         // need to check if businfo record is valid?
         if (streq(businfo->drm_connector_name, connector_name)) {
            busno = businfo->busno;
            break;
         }
      }
   }
   DBGTRC_DONE(debug, DDCA_TRC_NONE, "returning busno %d", busno);
   return busno;
}




/** Simpler alternative to #ddc_i2c_check_bus_changes() for the common case where
 *  all displays have a sysfs connector record with an accurate edid attribute.
 *
 *  Checks whether the edid attribute in the card-connector directory exists,
 *  and determines whether the list of buses with edid in
 *  **bs_prev_buses_w_edid** has changed.
 *
 *  If differences exist, either emit events directly or place them on the
 *  deferred events queue.
 *
 *  @param connector_number     drm connector number
 *  @param connector_name       name of card-connector directory
 *  @param bs_prev_buses_w_edid  previous set of buses have edid
 *  @param events_queue          if null, emit events directly
 *                               if non-null, put events on the queue
 *  @return updated set of buses having edid
 */
Bit_Set_256 ddc_i2c_check_bus_changes_for_connector(
      int         connector_number,
      char *      connector_name,
      Bit_Set_256 bs_prev_buses_w_edid,
      GArray *    events_queue)
{
   bool debug = false;
   // int debug_depth = (debug) ? 1 : -1;
   DBGTRC_STARTING(debug, DDCA_TRC_NONE, "connector_number=%d, connector_name=%s, bs_prev_buses_w_edid: %s",
         connector_number, connector_name, BS256_REPR(bs_prev_buses_w_edid));

   Bit_Set_256 bs_new_buses_w_edid = bs_prev_buses_w_edid;
   int busno = search_all_businfo_record_by_connector_name(connector_name);
   // busno -1 possible for added hub devices, only the one w attached monitor will have busno
   if (busno < 0)
      goto bye;

   bool prior_has_edid = bs256_contains(bs_prev_buses_w_edid, busno);
   bool stabilized_bus_has_edid =
        // ddc_i2c_stabilized_bus_by_connector_id(connector_number, prior_has_edid);
        ddc_i2c_stabilized_single_bus_by_connector_name(connector_name, prior_has_edid);
   DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE,
         "ddc_i2c_stabilized_bus_by_connector_id() returned %s",
         SBOOL(stabilized_bus_has_edid));
   if (stabilized_bus_has_edid != prior_has_edid) {
      if (stabilized_bus_has_edid) {
         bs_new_buses_w_edid = bs256_insert(bs_new_buses_w_edid, busno);
      }
      else {
         bs_new_buses_w_edid = bs256_remove(bs_new_buses_w_edid, busno);
      }
   }

   bool hotplug_change_handler_emitted = false;
   bool connected_buses_changed = !bs256_eq( bs_prev_buses_w_edid, bs_new_buses_w_edid);
   DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "connected_buses_changed = %s", SBOOL(connected_buses_changed));

   if (connected_buses_changed) {
      BS256 bs_buses_w_edid_removed = bs256_and_not(bs_prev_buses_w_edid, bs_new_buses_w_edid);
      DBGTRC_NOPREFIX(debug, TRACE_GROUP, "bs_buses_w_edid_removed: %s", BS256_REPR(bs_buses_w_edid_removed));

      BS256 bs_buses_w_edid_added = bs256_and_not(bs_new_buses_w_edid, bs_prev_buses_w_edid);
      DBGTRC_NOPREFIX(debug, TRACE_GROUP, "bs_buses_w_edid_added: %s", BS256_REPR(bs_buses_w_edid_added));

      hotplug_change_handler_emitted = ddc_i2c_hotplug_change_handler(
                                           bs_buses_w_edid_removed,
                                           bs_buses_w_edid_added,
                                           events_queue);
   }

   if (hotplug_change_handler_emitted)
      DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "hotplug_change_handler_emitted = %s",
            sbool (hotplug_change_handler_emitted));

 bye:
   DBGTRC_DONE(debug, TRACE_GROUP, "Returning Bit_Set_256: %s", BS256_REPR(bs_new_buses_w_edid));
   return bs_new_buses_w_edid;
}


#ifdef BAD
Bit_Set_256 ddc_i2c_check_bus_changes_for_connector(
      int         connector_number,
      char *      connector_name,
      Bit_Set_256 bs_prev_buses_w_edid,
      GArray *    events_queue)
{
   bool debug = true;
   int debug_depth = (debug) ? 1 : -1;
   DBGTRC_STARTING(debug, DDCA_TRC_NONE, "connector_number=%d, connector_name=%s, bs_prev_buses_w_edid: %s",
         connector_number, connector_name, BS256_REPR(bs_prev_buses_w_edid));

   Sys_Drm_Connector * conn = get_drm_connector(connector_name, debug_depth);   // can fail!
   int busno = conn->i2c_busno;
   free(conn);

   return ddc_i2c_check_bus_changes_for_busno(busno, bs_prev_buses_w_edid, events_queue);
}
#endif


typedef struct {
   const char * prop_subsystem;
   const char * prop_action;
   const char * prop_connector;
   const char * prop_devname;
   const char * prop_hotplug;
   const char * sysname;
   const char * attr_name;
} Udev_Event_Detail;


Udev_Event_Detail* collect_udev_event_detail(struct udev_device * dev) {
   Udev_Event_Detail * cd = calloc(1, sizeof(Udev_Event_Detail));
   cd->prop_subsystem = udev_device_get_property_value(dev, "SUBSYSTEM");
   cd->prop_action    = udev_device_get_property_value(dev, "ACTION");     // always "changed"
   cd->prop_connector = udev_device_get_property_value(dev, "CONNECTOR");  // drm connector number
   cd->prop_devname   = udev_device_get_property_value(dev, "DEVNAME");    // e.g. /dev/dri/card0
   cd->prop_hotplug   = udev_device_get_property_value(dev, "HOTPLUG");    // always 1
   cd->sysname        = udev_device_get_sysname(dev);                      // e.g. card0, i2c-27
   cd-> attr_name     = udev_device_get_sysattr_value(dev, "name");
   return cd;
}


void free_udev_event_detail(Udev_Event_Detail * detail) {
   free(detail);
}


void dbgrpt_udev_event_detail(Udev_Event_Detail * detail, int depth) {
   assert(detail);
   rpt_structure_loc("Udev_Event_Detail", detail, depth);
   int d1 = depth + 1;
   rpt_vstring(d1, "prop_subsystem:  %s", detail->prop_subsystem);
   rpt_vstring(d1, "prop_action:     %s", detail->prop_action);
   rpt_vstring(d1, "prop_connector:  %s", detail->prop_connector);
   rpt_vstring(d1, "prop_devname:    %s", detail->prop_devname);
   rpt_vstring(d1, "prop_hotplug:    %s", detail->prop_hotplug);
   rpt_vstring(d1, "sysname:         %s", detail->sysname);
   rpt_vstring(d1, "attr_name:       %s", detail->attr_name);
}


void xxx(char * msg) {
   if (msg)
      DBGMSG(msg);
   execute_shell_cmd("ls -l /sys/bus/i2c/devices/i2c* | grep 02:00");
   execute_shell_cmd("ls -l /sys/class/drm/card2-*");
}




void debug_watch_state(int connector_number, char * cname) {
   // NB needs validity checks for production
   bool debug = true;

   if (IS_DBGTRC(debug, DDCA_TRC_NONE)) {
      Sys_Drm_Connector * cur =  get_drm_connector(cname, 2);
      free_sys_drm_connector(cur);
   }

   get_sys_drm_connectors(true);
   rpt_vstring(1, "drm connectors");
   report_sys_drm_connectors(true, 1);
   Sys_Drm_Connector * conn = find_sys_drm_connector_by_connector_id(connector_number);
   rpt_vstring(1, "connector_number=%d, busno=%d, has_edid=%s",
         connector_number, conn->i2c_busno, sbool(conn->edid_bytes != NULL));

   rpt_label(0, "/sys/class/drm state after hotplug event:");
   dbgrpt_sysfs_basic_connector_attributes(1);
   if (use_drm_connector_states) {
      rpt_vstring(0, "DRM connector states after hotplug event:");
      report_drm_connector_states_basic(/*refresh*/ true, 1);
   }
}


/** Main loop watching for display changes. Runs as thread.
 *
 *  @param data   #Watch_Displays_Data passed from creator thread
 */
gpointer ddc_watch_displays_udev_i2c(gpointer data) {
   bool debug = false;
   bool debug_sysfs_state = false;
   bool use_deferred_event_queue = false;

   Watch_Displays_Data * wdd = data;
   assert(wdd && memcmp(wdd->marker, WATCH_DISPLAYS_DATA_MARKER, 4) == 0 );
   DBGTRC_STARTING(debug, DDCA_TRC_NONE,
         "Caller process id: %d, caller thread id: %d, event_classes=0x%02x",
         wdd->main_process_id, wdd->main_thread_id, wdd->event_classes);
   DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "Watching for display connection events: %s",
         sbool(wdd->event_classes & DDCA_EVENT_CLASS_DISPLAY_CONNECTION));
   DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "Watching for dpms events: %s",
          sbool(wdd->event_classes & DDCA_EVENT_CLASS_DPMS));

   bool watch_dpms = wdd->event_classes & DDCA_EVENT_CLASS_DPMS;

   pid_t cur_pid = getpid();
   pid_t cur_tid = get_thread_id();
   DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "Our process id: %d, our thread id: %d", cur_pid, cur_tid);

#ifdef NEVER_USED
   GPtrArray * sleepy_connectors = NULL;
   if (watch_dpms)
      sleepy_connectors = g_ptr_array_new_with_free_func(g_free);
#endif
   BS256 bs_sleepy_buses = EMPTY_BIT_SET_256;

   struct udev* udev = udev_new();
   struct udev_monitor* mon = udev_monitor_new_from_netlink(udev, "udev");
   // Alternative subsystem devtype values that did not detect changes:
   // drm_dp_aux_dev, kernel, i2c-dev, i2c, hidraw
    udev_monitor_filter_add_match_subsystem_devtype(mon, "drm", NULL);   // detects
   // testing for hub changes
// #ifdef UDEV_I2C_DEV
    // i2c-dev report i2c device number, i2c does not, but still not useful
    udev_monitor_filter_add_match_subsystem_devtype(mon, "i2c-dev", NULL);
// #endif
   // udev_monitor_filter_add_match_subsystem_devtype(mon, "i2c", NULL);
   udev_monitor_enable_receiving(mon);

   // make udev_monitor_receive_device() blocking
   // int fd = udev_monitor_get_fd(mon);
   // set_fd_blocking(fd);

   // Sysfs_Connector_Names current_connector_names = get_sysfs_drm_connector_names();
   Bit_Set_256 bs_cur_buses_w_edid =
         buses_bitset_from_businfo_array(all_i2c_buses, /*only_connected=*/ true);
   DBGTRC_NOPREFIX(debug, TRACE_GROUP, "Initial i2c buses with edids: %s",
         BS256_REPR(bs_cur_buses_w_edid));
   if (IS_DBGTRC(debug, DDCA_TRC_NONE)) {
      rpt_vstring(0, "Initial I2C buses:");
      i2c_dbgrpt_buses_summary(1);
      rpt_vstring(0, "Initial Display Refs:");
      ddc_dbgrpt_display_refs_summary(true,     // include_invalid_displays
                                      false,    // report_businfo
                                      1);       // depth
      if (use_drm_connector_states) {
         rpt_vstring(0, "Initial DRM connector states");
         report_drm_connector_states_basic(/*refresh*/ true, 1);
      }
   }

   GArray * deferred_events = NULL;
   if (use_deferred_event_queue)
      deferred_events = g_array_new( false,      // zero_terminated
                                     false,       // clear
                                     sizeof(DDCA_Display_Status_Event));
   // if (IS_DBGTRC(debug_sysfs_state, DDCA_TRC_NONE)) {
   if (debug_sysfs_state) {
      rpt_label(0, "Initial sysfs state:");
      dbgrpt_sysfs_basic_connector_attributes(1);
   }
   ASSERT_IFF(deferred_events, use_deferred_event_queue);

   struct udev_device * dev = NULL;
   time_t last_drm_change_timestamp = 0;
   bool skip_next_sleep = false;
   while (true) {
      if (wdd->event_classes & DDCA_EVENT_CLASS_DISPLAY_CONNECTION) {
         dev = udev_monitor_receive_device(mon);
      }
      if (dev) {
         DBGTRC(debug || report_udev_events, DDCA_TRC_NONE, "Udev event received");
      }

      while (!dev) {
         int slept = 0;   // will contain length of final sleep
         if (deferred_events && deferred_events->len > 0) {
            ddc_i2c_emit_deferred_events(deferred_events);
         }
         else {     // skip polling loop sleep if deferred events were output
            if (!skip_next_sleep) {
               int poll_loop_millisec = udev_poll_loop_millisec;
               if (ddc_slow_watch)   // for testing
                  poll_loop_millisec *= 3;
               const int max_sleep_microsec = poll_loop_millisec * 1000;
               const int sleep_step_microsec = MIN(200, max_sleep_microsec);     // .2 sec
               slept = 0;
               for (; slept < max_sleep_microsec && !terminate_watch_thread; slept += sleep_step_microsec)
                  usleep(sleep_step_microsec);
            }
         }
         skip_next_sleep = false;

         if (terminate_watch_thread) {
            // n. slept == 0 if no sleep was performed
            DBGTRC_DONE(debug, TRACE_GROUP,
                  "Terminating thread.  Final polling sleep was %d millisec.", slept/1000);
              free_watch_displays_data(wdd);
              //  int rc = udev_monitor_filter_remove(mon);
              udev_monitor_unref(mon);
              udev_unref(udev);
#ifdef WATCH_DPMS
              if (watch_dpms)
                 g_ptr_array_free(sleepy_connectors, true);
#endif
            g_thread_exit(0);
            assert(false);    // avoid clang warning re wdd use after free
         }

         if (watch_dpms) {
            // DBGTRC_NOPREFIX(debug, TRACE_GROUP, "Before ddc_check_bus_asleep(), bs_sleepy_buses: %s",
            //      BS256_REPR(bs_sleepy_buses));
            // emits dpms events directly or places them on deferred_events queue
            bs_sleepy_buses = ddc_i2c_check_bus_asleep(
                  bs_cur_buses_w_edid, bs_sleepy_buses, deferred_events);
            // DBGTRC_NOPREFIX(debug, TRACE_GROUP, "After ddc_check_bus_asleep(), bs_sleepy_buses: %s",
            //       BS256_REPR(bs_sleepy_buses));
         }

         // Doesn't work to detect client crash, main thread and process remains for some time.
         // 11/2020: is this even needed since terminate_watch_thread check added?
         // #ifdef DOESNT_WORK
         bool pid_found = is_valid_thread_or_process(cur_pid);
         if (!pid_found) {
            DBGMSG("Process %d not found", cur_pid);
         }
         bool tid_found = is_valid_thread_or_process(cur_tid);
         if (!pid_found || !tid_found) {
            DBGMSG("Thread %d not found", cur_tid);
            g_thread_exit(GINT_TO_POINTER(-1));
            break;
         }
         // #endif


         if (wdd->event_classes & DDCA_EVENT_CLASS_DISPLAY_CONNECTION) {
            dev = udev_monitor_receive_device(mon);
         }
         if (dev) {
            DBGTRC(debug || report_udev_events, DDCA_TRC_NONE, "Udev event received");
         }
      }  // end of udev_monitor_receive_dev() polling loop

      DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "==> udev_event received");
      assert(dev);

#ifdef DEBUGGING
      // WHICH REPORT TO USE?
      if (true) {
         DBGTRC_NOPREFIX(true, DDCA_TRC_NONE, "Got Device\n");
         dbgrpt_udev_device(dev, /*verbose=*/false, 2);
      }

      report_udev_device(dev, 3 );
#endif

      skip_next_sleep = true;

      Udev_Event_Detail * cd = collect_udev_event_detail(dev);
      assert(cd);      // mollify coverity scan
       if (IS_DBGTRC(debug || report_udev_events, DDCA_TRC_NONE))
         dbgrpt_udev_event_detail(cd, 2);
       // xxx("Event received");

      if (!streq(cd->prop_subsystem, "i2c-dev") &&  !streq(cd->prop_subsystem, "drm")) {
         DBGMSG("Unexpected subsystem: %s", cd->prop_subsystem);
      }

      else if (  streq(cd->prop_subsystem, "i2c-dev") && streq(cd->prop_action, "add") ) {
         // const char * sysname = cd->sysname;     // e.g i2c-27
         // const char * attr_name = cd->attr_name;
         int busno = i2c_name_to_busno(cd->sysname);
         if (busno < 0) {
            MSG_W_SYSLOG(DDCA_SYSLOG_ERROR, "sysname is not i2c-n");
         }
         else {
            bool new_businfo = false;
            I2C_Bus_Info * businfo =  i2c_find_bus_info_in_gptrarray_by_busno(all_i2c_buses, busno);
            if (businfo) {
               DBGMSG("Unexpected businfo record %p already exists for bus %d", businfo, busno);
               // TO DO: check for use in non-removed drefs
               i2c_reset_bus_info(businfo);
            }
            if (!businfo) {
               businfo = i2c_new_bus_info(busno);
               new_businfo = true;
            }
            i2c_check_bus2(businfo);
            // Error_Info * err = i2c_check_bus2(businfo);
            // ERRINFO_FREE_WITH_REPORT(err, debug || IS_TRACING() || report_freed_exceptions);
            i2c_dbgrpt_bus_info(businfo, /*include_sysinfo*/ true, 0);

            if (new_businfo) {
               DBGTRC_NOPREFIX(true, DDCA_TRC_NONE, "Adding /dev/"I2C"-%d to list of buses", busno);
               i2c_add_bus_info(businfo);
            }
         }
      }

      else if ( streq(cd->prop_subsystem, "drm") &&
                 streq(cd->prop_action,   "add") ) {
         DBGTRC_NOPREFIX(true, DDCA_TRC_NONE, "Processing subsystem drm, action add");
         Bit_Set_256  bs_udev_buses =    i2c_detect_attached_buses_as_bitset();
         Bit_Set_256  bs_known_buses = EMPTY_BIT_SET_256;
         for (int ndx = 0; ndx < all_i2c_buses->len; ndx++) {
            I2C_Bus_Info * cur = g_ptr_array_index(all_i2c_buses, ndx);
            // need to check if valid?
            bs_known_buses = bs256_insert(bs_known_buses, cur->busno);
         }

         DBGTRC_NOPREFIX(true, DDCA_TRC_NONE, "udev buses: %s", BS256_REPR(bs_udev_buses));
         DBGTRC_NOPREFIX(true, DDCA_TRC_NONE, "known_buses: %s", BS256_REPR(bs_known_buses));

         BS256 buses_added = bs256_minus(bs_udev_buses, bs_known_buses);
         DBGTRC_NOPREFIX(true, DDCA_TRC_NONE, "Buses added: %s", BS256_REPR(buses_added));
         if (bs256_count(buses_added) > 0) {
            Bit_Set_256_Iterator iter = bs256_iter_new(buses_added);
            while (true) {
               int busno = bs256_iter_next(iter);
               if (busno < 0)
                  break;
               I2C_Bus_Info * businfo = i2c_new_bus_info(busno);
               i2c_check_bus2(businfo);
               i2c_dbgrpt_bus_info(businfo, /* include_sysinfo */ true, 2);
               DBGTRC_NOPREFIX(true, DDCA_TRC_NONE, "Adding businfo record for /dev/"I2C"-%d", busno);
               i2c_add_bus_info(businfo);
            }
         }
      }

      else if ( streq(cd->prop_subsystem,  "drm") &&
                streq(cd->prop_action,     "change") ) {
         // xxx("drm change");
         bool processed = false;
         time_t prev_change_timestamp = last_drm_change_timestamp;
         last_drm_change_timestamp = cur_realtime_nanosec();
         time_t delta_time = last_drm_change_timestamp - prev_change_timestamp;
         DBGTRC_NOPREFIX(true, DDCA_TRC_NONE, "nanosec since prev drm/change event: %jd", delta_time);
         if (use_sysfs_connector_id) {
            char * cname = NULL;
            int connector_number = -1;
            if (cd && streq(cd->prop_action, "change")
                                && cd->prop_connector) {  // seen null when MST hub added
               bool valid_number = str_to_int(cd->prop_connector, &connector_number, 10);
               assert(valid_number);
               cname = get_sys_drm_connector_name_by_connector_id(connector_number);
               DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE,
                     "get_sys_drm_connector_name_by_connector_id() returned: %s", cname);

               if (debug_sysfs_state) {   // move debug statements out of mainline
                  debug_watch_state(connector_number, cname);
               }  // debug_sysfs_state

               if (cname) {
                  DBGTRC_NOPREFIX(true, DDCA_TRC_NONE, "1) Using connector id %d, name =%s", connector_number, cname);
                  bs_cur_buses_w_edid = ddc_i2c_check_bus_changes_for_connector(
                                        connector_number, cname, bs_cur_buses_w_edid, deferred_events);
                  // xxx("drm change case1");
                  processed = true;
               }
   #ifdef UDEV_I2C_DEV
               if (!processed) {
                  if (drm_udev_detail &&
                        (streq(drm_udev_detail->prop_action,"add")||streq(drm_udev_detail->prop_action, "remove") )
                        && drm_udev_detail->sysname)
                  {
                     int busno = i2c_name_to_busno(drm_udev_detail->sysname);
                     cname = get_sys_drm_connector_name_by_busno(busno);
                  }
                  if (cname) {
                     DBGTRC(true, DDCA_TRC_NONE,
                           "2) connector name reported by get_sys_drm_connector_name_by_busno(): %s",
                           cname);
                     bs_cur_buses_w_edid = ddc_i2c_check_bus_changes_for_connector(
                                        connector_number, cname, bs_cur_buses_w_edid, deferred_events);
                     processed = true;
                  }
               }
               if (!processed) {
                  if (i2c_dev_udev_detail && i2c_dev_udev_detail->sysname
                      && (streq(drm_udev_detail->prop_action,"add")||streq(drm_udev_detail->prop_action, "remove")) )
                  {
                     int busno = i2c_name_to_busno(i2c_dev_udev_detail->sysname);
                     cname = get_sys_drm_connector_name_by_busno(busno);
                  }
                  if (cname) {
                     DBGTRC_NOPREFIX(true, DDCA_TRC_NONE,
                           "3) connector name reported by get_sys_drm_connector_name_by_busno(): %s", cname);
                     bs_cur_buses_w_edid = ddc_i2c_check_bus_changes_for_connector(
                                        connector_number, cname, bs_cur_buses_w_edid, deferred_events);
                     processed = true;
                  }
               }
   #endif
            }
            free(cname);
         }

         if (!processed) {
            DBGTRC_NOPREFIX(true, DDCA_TRC_NONE, "4) Calling ddc_i2c_check_bus_changes");
            // emits display change events or queues them
            bs_cur_buses_w_edid = ddc_i2c_check_bus_changes(bs_cur_buses_w_edid, deferred_events);
         }

        if (watch_dpms) {
           // remove buses marked asleep if they no longer have a monitor so they will
           // not be considered asleep when reconnected
           bs_sleepy_buses = bs256_and(bs_sleepy_buses, bs_cur_buses_w_edid);
        }
      } // subsystem drm, action change

      free_udev_event_detail(cd);
      cd = NULL;
      udev_device_unref(dev);
      dev = NULL;

      DBGTRC_NOPREFIX(debug, DDCA_TRC_NONE, "==> udev event processed");
   }  // while

   // This is not the real function exit point.  Function termination occurs
   // using g_thread_exit within the udev event polling loop.
   assert(false);
   return NULL;
}


#endif // ENABLE_UDEV


void init_ddc_watch_displays() {


#ifdef ENABLE_UDEV
#ifdef UNUSED
   RTTI_ADD_FUNC(ddc_i2c_filter_sleep_events);
#endif
   RTTI_ADD_FUNC(search_all_businfo_record_by_connector_name);
   RTTI_ADD_FUNC(ddc_i2c_check_bus_changes);
   RTTI_ADD_FUNC(ddc_i2c_check_bus_changes_for_connector);
   RTTI_ADD_FUNC(ddc_i2c_stabilized_buses);
   RTTI_ADD_FUNC(ddc_i2c_stabilized_bus_by_connector_id);
   RTTI_ADD_FUNC(ddc_i2c_stabilized_single_bus_by_connector_name);
   RTTI_ADD_FUNC(ddc_i2c_check_bus_asleep);
   RTTI_ADD_FUNC(ddc_watch_displays_udev_i2c);
#endif
}
