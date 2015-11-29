/*
 * ddc_strategy.h
 *
 *  Created on: Nov 21, 2015
 *      Author: rock
 */

#ifndef DDC_STRATEGY_H_
#define DDC_STRATEGY_H_

#include "util/coredefs.h"

#include "base/displays.h"
#include "base/status_code_mgt.h"


// For future use

void init_ddc_strategies();


typedef Global_Status_Code (*DDC_Raw_Writer)(Display_Handle * dh, int bytect, Byte * bytes);
typedef Global_Status_Code (*DDC_Raw_Reader)(Display_Handle * dh, int bufsize, Byte * buffer);

typedef struct {
   DDC_IO_Mode     io_mode;
   DDC_Raw_Writer  writer;
   DDC_Raw_Reader  reader;
}  DDC_Strategy;

DDC_Raw_Writer ddc_raw_writer(Display_Handle * dh);
DDC_Raw_Reader ddc_raw_reader(Display_Handle * dh);


#endif /* DDC_STRATEGY_H_ */
