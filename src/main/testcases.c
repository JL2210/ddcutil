/*  testcases.c
 *
 *  Created on: Oct 27, 2015
 *      Author: rock
 *
 *  Dispatch test cases
 */

#include <config.h>
#include <stdio.h>

#include "base/displays.h"
#include "base/util.h"

#include "adl/adl_shim.h"
#ifdef HAVE_ADL
#include "adl/adl_impl/adl_intf.h"
#endif

#include "test/testcase_table.h"

#include "main/testcases.h"


void show_test_cases() {
   printf("\n Test Cases:\n");
   int ndx = 0;
   // int testcase_catalog_ct = get_testcase_catalog_ct();
   // Testcase_Descriptor ** testcase_catalog = get_testcase_catalog();
   for (;ndx < testcase_catalog_ct; ndx++) {
      printf("  %d - %s\n", ndx+1, testcase_catalog[ndx].name);
   }
   puts("");
}


Testcase_Descriptor * get_testcase_descriptor(int testnum) {
   Testcase_Descriptor * result = NULL;
   // int testcase_catalog_ct = get_testcase_catalog_ct();
   // Testcase_Descriptor ** testcase_catalog = get_testcase_catalog();
   if (testnum > 0 && testnum <= testcase_catalog_ct) {
      result = &testcase_catalog[testnum-1];
   }
   return result;
}

bool execute_testcase(int testnum, Display_Identifier* pdid) {
      bool ok = true;
      Testcase_Descriptor * pDesc = NULL;

      if (ok) {
         pDesc = get_testcase_descriptor(testnum);
         if (!pDesc) {
            printf("Invalid test number: %d\n", testnum);
            ok = false;
         }
      }

      if (ok) {
         if (pdid->id_type == DISP_ID_ADL && !adlshim_is_available()) {
            printf("ADL adapter.display numbers specified, but ADL is not available.\n");
            ok = false;
         }
      }

      if (ok) {
         switch (pDesc->drefType) {

         case DisplayRefNone:
            pDesc->fp_noarg();
            break;

         case DisplayRefBus:
            // if (parsedCmd->dref->ddc_io_mode == DDC_IO_ADL) {
            if (pdid->id_type != DISP_ID_BUSNO) {
               printf("Test %d requires bus number\n", testnum);
               ok = false;
            }
            else {
               // pDesc->fp_bus(parsedCmd->dref->busno);
               pDesc->fp_bus(pdid->busno);
            }
            break;

         case DisplayRefAdl:
             // if (parsedCmd->dref->ddc_io_mode == DDC_IO_DEVI2C) {
             if (pdid->id_type != DISP_ID_ADL) {
                printf("Test %d requires ADL adapter.display numbers\n", testnum);
                ok = false;
             }
             else {
                // pDesc->fp_adl(parsedCmd->dref->iAdapterIndex, parsedCmd->dref->iDisplayIndex);
                pDesc->fp_adl(pdid->iAdapterIndex, pdid->iDisplayIndex);
             }
             break;

         case DisplayRefAny:
            {
               // pDesc->fp_dr(parsedCmd->dref);
               Display_Ref* pdref = NULL;
               if (pdid->id_type == DISP_ID_ADL) {
                  pdref = create_adl_display_ref(pdid->iAdapterIndex, pdid->iDisplayIndex);
               }
               else {
                  pdref = create_bus_display_ref(pdid->busno);
               }
               pDesc->fp_dr(pdref);
            }
            break;

         default:
            PROGRAM_LOGIC_ERROR("Impossible display id type: %d\n", pDesc->drefType);
         }  // switch
      }

     return ok;
}
