/*
  gcode.c - rs274/ngc parser.
  Part of LasaurGrbl

  Copyright (c) 2009-2011 Simen Svale Skogsrud
  Copyright (c) 2011 Stefan Hechenberger
  Copyright (c) 2011 Sungeun K. Jeon
  
  Inspired by the Arduino GCode Interpreter by Mike Ellery and the
  NIST RS274/NGC Interpreter by Kramer, Proctor and Messina.  

  LasaurGrbl is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  LasaurGrbl is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
*/

#include <string.h>
#include <math.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

#include <inc/hw_types.h>
#include <inc/hw_memmap.h>
#include <inc/hw_ints.h>
#include <inc/hw_gpio.h>

#include <driverlib/gpio.h>

#include "config.h"

#include "gcode.h"
#include "serial.h"
#include "sense_control.h"
#include "planner.h"
#include "stepper.h"
#include "temperature.h"

#define MM_PER_INCH (25.4)

enum {
	NEXT_ACTION_NONE = 0,
	NEXT_ACTION_SEEK,
	NEXT_ACTION_FEED,
	NEXT_ACTION_DWELL,
	NEXT_ACTION_HOMING_CYCLE,
	NEXT_ACTION_SET_COORDINATE_OFFSET,
	NEXT_ACTION_AIR_ASSIST_ENABLE,
	NEXT_ACTION_AIR_ASSIST_DISABLE,
	NEXT_ACTION_AUX1_ASSIST_ENABLE,
	NEXT_ACTION_AUX1_ASSIST_DISABLE,
	NEXT_ACTION_SET_ACCELERATION,
};

#define OFFSET_G54 0
#define OFFSET_G55 1

#define BUFFER_LINE_SIZE 80

uint8_t gcode_ready_wait = 0;

static char rx_line[BUFFER_LINE_SIZE];
static int rx_chars = 0;
static char *rx_line_cursor;

static uint8_t line_checksum_ok_already;

#define FAIL(status) gc.status_code = status;

typedef struct {
  uint8_t status_code;             // return codes
  uint8_t motion_mode;             // {G0, G1}
  bool inches_mode;                // 0 = millimeter mode, 1 = inches mode {G20, G21}
  bool absolute_mode;              // 0 = relative motion, 1 = absolute motion {G90, G91}
  double feed_rate;                // mm/min {F}
  double seek_rate;                // mm/min {F}
  double position[3];              // projected position once all scheduled motions will have been executed
  double offsets[6];               // coord system offsets {G54_X,G54_Y,G54_Z,G55_X,G55_Y,G55_Z}
  uint8_t offselect;               // currently active offset, 0 -> G54, 1 -> G55
  uint8_t nominal_laser_intensity; // 0-255 percentage
  double acceleration;			   // mm/min/min
} parser_state_t;
static parser_state_t gc;

static volatile bool position_update_requested;  // make sure to update to stepper position on next occasion

// prototypes for static functions (non-accesible from other files)
static int next_statement(char *letter, double *double_ptr, char *line, uint8_t *char_counter);
static int read_double(char *line, uint8_t *char_counter, double *double_ptr);


void gcode_init() {
  memset(&gc, 0, sizeof(gc));
  gc.feed_rate = CONFIG_MAX_FEEDRATE;
  gc.seek_rate = CONFIG_MAX_SEEKRATE;
  gc.acceleration = CONFIG_DEFAULT_ACCELERATION;
  gc.absolute_mode = true;
  gc.nominal_laser_intensity = 0U;   
  gc.offselect = OFFSET_G54;
  // prime G54 cs
  // refine with "G10 L2 P0 X_ Y_ Z_"
  gc.offsets[X_AXIS] = CONFIG_X_ORIGIN_OFFSET;
  gc.offsets[Y_AXIS] = CONFIG_Y_ORIGIN_OFFSET;
  gc.offsets[Z_AXIS] = CONFIG_Z_ORIGIN_OFFSET;
  // prime G55 cs
  // refine with "G10 L2 P1 X_ Y_ Z_"
  // or set to any current location with "G10 L20 P1"
  gc.offsets[3+X_AXIS] = CONFIG_X_ORIGIN_OFFSET;
  gc.offsets[3+Y_AXIS] = CONFIG_Y_ORIGIN_OFFSET;
  gc.offsets[3+Z_AXIS] = CONFIG_Z_ORIGIN_OFFSET;
  position_update_requested = false;
  line_checksum_ok_already = false; 
}

void gcode_process_data(const tUSBBuffer *psBuffer)
{
	uint8_t chr = '\0';

	if (planner_blocks_available() < 2) {
		//gcode_ready_wait = 1;
		return;
	}

	// Read all data available...
	while (USBBufferRead(psBuffer, &chr, 1) == 1)
	{
		if ((chr == 0x0A) || (chr == 0x0D)) {
			//// process line
			if (rx_chars > 0) {          // Line is complete. Then execute!
				rx_line[rx_chars] = '\0';  // terminate string
				gcode_process_line(rx_line, rx_chars);
				rx_chars = 0;
			}
		}
		else if (rx_chars + 1 >= BUFFER_LINE_SIZE) {
			// reached line size, other side sent too long lines
			stepper_request_stop(STATUS_LINE_BUFFER_OVERFLOW);
			break;
		}
		else if (chr == 0x14) {
			// Respond to Lasersaur's ready request
    		if (planner_blocks_available() >= 2) {
				char tmp[2] = {0x12, 0};
				printString(tmp);
			}
    		else {
    			// We can't wait here (ISR context) set a flag so that the main thread
    			// sends a response when planner blocks become free.
    			gcode_ready_wait = 1;
    		}
		} else if (chr <= 0x20) {
			// ignore control characters and space
		} else {
			// add to line, as char which is signed
			rx_line[rx_chars++] = (char)chr;
		}
	}
}


void gcode_process_line(char *buffer, int length) {
  int status_code = STATUS_OK;
  uint8_t skip_line = 0;
  uint8_t print_extended_status = 0;

    // handle position update after a stop
    if (position_update_requested) {
      gc.position[X_AXIS] = stepper_get_position_x();
      gc.position[Y_AXIS] = stepper_get_position_y();
      gc.position[Z_AXIS] = stepper_get_position_z();
      position_update_requested = false;
      //printString("gcode pos update\n");  // debug
    }
        
    if (stepper_stop_requested()) {
      printString("!");  // report harware is in stop mode
      status_code = stepper_stop_status();
      // report stop conditions
      if ( status_code == STATUS_POWER_OFF) {
        printString("P");  // Stop: Power Off
      } else if (status_code == STATUS_LIMIT_HIT) {
        printString("L");  // Stop: Limit Hit
        stepper_stop_resume();
        gcode_do_home();
      } else if (status_code == STATUS_SERIAL_STOP_REQUEST) {
        printString("R");  // Stop: Serial Request   
      } else if (status_code == STATUS_RX_BUFFER_OVERFLOW) {
        printString("B");  // Stop: Rx Buffer Overflow  
      } else if (status_code == STATUS_LINE_BUFFER_OVERFLOW) {
        printString("I");  // Stop: Line Buffer Overflow  
      } else if (status_code == STATUS_TRANSMISSION_ERROR) {
        printString("T");  // Stop: Serial Transmission Error  
      } else {
        printString("O");  // Stop: Other error
        printInteger(status_code);        
      }
    } else {
      if (buffer[0] == '*' || buffer[0] == '^') {
        // receiving a line with checksum
        // expecting 0-n redundant lines starting with '^'
        // followed by a final line prepended by '*'
        if (!line_checksum_ok_already) {
          rx_line_cursor = buffer+2;  // set line offset
          uint8_t rx_checksum = (uint8_t)buffer[1];
          if (rx_checksum < 128) {
            printString(buffer);
            printString(" -> checksum outside [128,255]");
            stepper_request_stop(STATUS_TRANSMISSION_ERROR);
          }
          char *itr = rx_line_cursor;
          uint16_t checksum = 0;
          while (*itr) {  // all chars without 0-termination
            checksum += (uint8_t)*itr++;
            if (checksum >= 128) {
              checksum -= 128;
            }          
          }
          checksum = (checksum >> 1) + 128; //  /2, +128
          // printString("(");
          // printInteger(rx_checksum);
          // printString(",");
          // printInteger(checksum);
          // printString(")");        
          if (checksum != rx_checksum) {
            if (buffer[0] == '^') {
              skip_line = true;
              printString("^");
            } else {  // '*'
              printString(buffer);
              stepper_request_stop(STATUS_TRANSMISSION_ERROR);
              // line_checksum_ok_already = false;
            }
          } else {  // we got a good line
            // printString("$");
            if (buffer[0] == '^') {
              line_checksum_ok_already = true;
            }            
            skip_line = false;
          }
        } else {  // we already got a correct line
          // printString("&");
          skip_line = true;
          if (buffer[0] == '*') {  // last redundant line
            line_checksum_ok_already = false;
          }
        }
      } else {
        rx_line_cursor = buffer;
      }
      
      if (!skip_line) {
        if (rx_line_cursor[0] != '?') {
          // process the next line of G-code
          status_code = gcode_execute_line(rx_line_cursor);
          // report parse errors
          if (status_code == STATUS_OK) {
            // pass
          } else if (status_code == STATUS_BAD_NUMBER_FORMAT) {
            printString("N");  // Warning: Bad number format
          } else if (status_code == STATUS_EXPECTED_COMMAND_LETTER) {
            printString("E");  // Warning: Expected command letter
          } else if (status_code == STATUS_UNSUPPORTED_STATEMENT) {
            printString("U");  // Warning: Unsupported statement   
          } else {
            printString("W");  // Warning: Other error
            printInteger(status_code);        
          } 
        } else {
          print_extended_status = true;
        } 
      }  
    }

    #ifndef DEBUG_IGNORE_SENSORS
      //// door and chiller status
      if (SENSE_DOOR_OPEN) {
        printString("D");  // Warning: Door is open
      }
      if (SENSE_CHILLER_OFF) {
          printString("C");  // Warning: Chiller is off
      }
      // limit
      if (SENSE_LIMITS) {
        if (SENSE_X_LIMIT) {
          printString("L1");  // Limit X Hit
        }
        if (SENSE_Y_LIMIT) {
          printString("L2");  // Limit Y Hit
        }
        if (SENSE_Z_LIMIT) {
          printString("L3");  // Limit Z Hit
        }
        if (SENSE_E_LIMIT) {
          printString("L4");  // E Stop Hit
        }
      } 
    #endif

    //
    if (print_extended_status) {
      // position
      printString("X");
      printFloat(stepper_get_position_x());
      printString("Y");
      printFloat(stepper_get_position_y());       
      // version
      printPgmString("V" LASAURGRBL_VERSION);
    }
    printString("\n");
}



// Executes one line of 0-terminated G-Code. The line is assumed to contain only uppercase
// characters and signed floating point values (no whitespace). Comments and block delete
// characters have been removed.
uint8_t gcode_execute_line(char *line) {
  uint8_t char_counter = 0;  
  char letter;
  double value;
  int int_value;
  double unit_converted_value;  
  uint8_t next_action = NEXT_ACTION_NONE;
  double target[3];
  double p = 0.0;
  int cs = 0;
  int l = 0;
  bool got_actual_line_command = false;  // as opposed to just e.g. G1 F1200
  gc.status_code = STATUS_OK;
    
  //// Pass 1: Commands
  while(next_statement(&letter, &value, line, &char_counter)) {
    int_value = trunc(value);
    switch(letter) {
      case 'G':
        switch(int_value) {
          case 0: gc.motion_mode = next_action = NEXT_ACTION_SEEK; break;
          case 1: gc.motion_mode = next_action = NEXT_ACTION_FEED; break;
          case 4: next_action = NEXT_ACTION_DWELL; break;
          case 10: next_action = NEXT_ACTION_SET_COORDINATE_OFFSET; break;
          case 20: gc.inches_mode = true; break;
          case 21: gc.inches_mode = false; break;
          case 28: next_action = NEXT_ACTION_HOMING_CYCLE; break;
          case 30: next_action = NEXT_ACTION_HOMING_CYCLE; break;
          case 54: gc.offselect = OFFSET_G54; break;
          case 55: gc.offselect = OFFSET_G55; break;
          case 90: gc.absolute_mode = true; break;
          case 91: gc.absolute_mode = false; break;
          default: FAIL(STATUS_UNSUPPORTED_STATEMENT); break;
        }
        break;
      case 'M':
        switch(int_value) {
          case 17: stepper_wake_up();break;
          case 18: stepper_request_stop(STATUS_SERIAL_STOP_REQUEST);break;
          case 80: next_action = NEXT_ACTION_AIR_ASSIST_ENABLE;break;
          case 81: next_action = NEXT_ACTION_AIR_ASSIST_DISABLE;break;
          case 82: next_action = NEXT_ACTION_AUX1_ASSIST_ENABLE;break;
          case 83: next_action = NEXT_ACTION_AUX1_ASSIST_DISABLE;break;
          case 105: printString("ok T:"); printFloat(temperature_read(0)/16.0);printString(" B:"); printFloat(temperature_read(1)/16.0); printString("\n");break;
          case 106: next_action = NEXT_ACTION_AIR_ASSIST_ENABLE;break;
          case 107: next_action = NEXT_ACTION_AIR_ASSIST_DISABLE;break;
          case 114: printString("ok C: X:"); printFloat(stepper_get_position_x()); printString(" Y:"); printFloat(stepper_get_position_y()); printString(" Z:"); printFloat(stepper_get_position_z()); printString("\n"); break;
          case 204: next_action = NEXT_ACTION_SET_ACCELERATION; break;
          default: FAIL(STATUS_UNSUPPORTED_STATEMENT); break;
        }            
        break;
    }
    if (gc.status_code) { break; }
  }
  
  // bail when errors
  if (gc.status_code) { return gc.status_code; }

  char_counter = 0;
  memcpy(target, gc.position, sizeof(target)); // i.e. target = gc.position

  //// Pass 2: Parameters
  while(next_statement(&letter, &value, line, &char_counter)) {
    if (gc.inches_mode) {
      unit_converted_value = value * MM_PER_INCH;
    } else {
      unit_converted_value = value;
    }
    switch(letter) {
      case 'F':
        if (unit_converted_value <= 0) { FAIL(STATUS_BAD_NUMBER_FORMAT); }
        if (gc.motion_mode == NEXT_ACTION_SEEK) {
          gc.seek_rate = min(CONFIG_MAX_SEEKRATE, unit_converted_value);
        } else {
          gc.feed_rate = min(CONFIG_MAX_FEEDRATE, unit_converted_value);
        }
        break;
      case 'X': case 'Y': case 'Z':
        if (gc.absolute_mode) {
          target[letter - 'X'] = unit_converted_value;
        } else {
          target[letter - 'X'] += unit_converted_value;
        }
        got_actual_line_command = true;
        break;        
      case 'P':  // dwelling seconds or CS selector
        if (next_action == NEXT_ACTION_SET_COORDINATE_OFFSET) {
          cs = trunc(value);
        } else {
          p = value;
        }
        break;
      case 'S':
    	  if (next_action == NEXT_ACTION_SET_ACCELERATION)
    		  gc.acceleration = value * 3600;
    	  else
    		  gc.nominal_laser_intensity = value;
        break; 
      case 'L':  // G10 qualifier 
      l = trunc(value);
        break;
    }
  }
  
  // bail when error
  if (gc.status_code) { return(gc.status_code); }
      
  //// Perform any physical actions
  switch (next_action) {
    case NEXT_ACTION_SEEK:
      if (got_actual_line_command) {
        planner_line( target[X_AXIS] + gc.offsets[3*gc.offselect+X_AXIS], 
                      target[Y_AXIS] + gc.offsets[3*gc.offselect+Y_AXIS], 
                      target[Z_AXIS] + gc.offsets[3*gc.offselect+Z_AXIS], 
                      gc.seek_rate, gc.acceleration, 0 );
      }
      break;   
    case NEXT_ACTION_FEED:
      if (got_actual_line_command) {
        planner_line( target[X_AXIS] + gc.offsets[3*gc.offselect+X_AXIS], 
                      target[Y_AXIS] + gc.offsets[3*gc.offselect+Y_AXIS], 
                      target[Z_AXIS] + gc.offsets[3*gc.offselect+Z_AXIS], 
                      gc.feed_rate, gc.acceleration, gc.nominal_laser_intensity );
      }
      break; 
    case NEXT_ACTION_DWELL:
      planner_dwell(p, gc.nominal_laser_intensity);
      break;
    // case NEXT_ACTION_STOP:
    //   planner_stop();  // stop and cancel the remaining program
    //   gc.position[X_AXIS] = stepper_get_position_x();
    //   gc.position[Y_AXIS] = stepper_get_position_y();
    //   gc.position[Z_AXIS] = stepper_get_position_z();
    //   planner_set_position(gc.position[X_AXIS], gc.position[Y_AXIS], gc.position[Z_AXIS]);
    //   // move to table origin
    //	 clear_vector(target);
    //   planner_line( target[X_AXIS] + gc.offsets[3*gc.offselect+X_AXIS], 
    //                 target[Y_AXIS] + gc.offsets[3*gc.offselect+Y_AXIS], 
    //                 target[Z_AXIS] + gc.offsets[3*gc.offselect+Z_AXIS], 
    //                 gc.seek_rate, 0 );
    //   break;
    case NEXT_ACTION_HOMING_CYCLE:
        clear_vector(target);
    	gcode_do_home();
      break;
    case NEXT_ACTION_SET_COORDINATE_OFFSET:
      if (cs == OFFSET_G54 || cs == OFFSET_G55) {
        if (l == 2) {
          //set offset to target, eg: G10 L2 P1 X15 Y15 Z0
          gc.offsets[3*cs+X_AXIS] = target[X_AXIS];
          gc.offsets[3*cs+Y_AXIS] = target[Y_AXIS];
          gc.offsets[3*cs+Z_AXIS] = target[Z_AXIS];
          // Set target in ref to new coord system so subsequent moves are calculated correctly.
          target[X_AXIS] = (gc.position[X_AXIS] + gc.offsets[3*gc.offselect+X_AXIS]) - gc.offsets[3*cs+X_AXIS];
          target[Y_AXIS] = (gc.position[Y_AXIS] + gc.offsets[3*gc.offselect+Y_AXIS]) - gc.offsets[3*cs+Y_AXIS];
          target[Z_AXIS] = (gc.position[Z_AXIS] + gc.offsets[3*gc.offselect+Z_AXIS]) - gc.offsets[3*cs+Z_AXIS];
          
        } else if (l == 20) {
          // set offset to current pos, eg: G10 L20 P2
          gc.offsets[3*cs+X_AXIS] = gc.position[X_AXIS] + gc.offsets[3*gc.offselect+X_AXIS];
          gc.offsets[3*cs+Y_AXIS] = gc.position[Y_AXIS] + gc.offsets[3*gc.offselect+Y_AXIS];
          gc.offsets[3*cs+Z_AXIS] = gc.position[Z_AXIS] + gc.offsets[3*gc.offselect+Z_AXIS];
          target[X_AXIS] = 0;
          target[Y_AXIS] = 0;
          target[Z_AXIS] = 0;                 
        }
      }
      break;
    case NEXT_ACTION_AIR_ASSIST_ENABLE:
      planner_control_air_assist_enable();
      break;
    case NEXT_ACTION_AIR_ASSIST_DISABLE:
      planner_control_air_assist_disable();
      break;
    case NEXT_ACTION_AUX1_ASSIST_ENABLE:
      planner_control_aux1_assist_enable();
      break;
    case NEXT_ACTION_AUX1_ASSIST_DISABLE:
      planner_control_aux1_assist_disable();
      break;
  }
  
  // As far as the parser is concerned, the position is now == target. In reality the
  // motion control system might still be processing the action and the real tool position
  // in any intermediate location.
  memcpy(gc.position, target, sizeof(double)*3); // gc.position[] = target[];
  return gc.status_code;
}


void gcode_request_position_update() {
  position_update_requested = true;
}

// Move by the supplied offset(s).
// Used by the joystick to move the head manually.
void gcode_manual_move(double x, double y) {
	gc.position[X_AXIS] += x;
	gc.position[Y_AXIS] += y;

	//gc.position[X_AXIS]=max(gc.position[X_AXIS], CONFIG_X_MIN);
	//gc.position[X_AXIS]=min(gc.position[X_AXIS], CONFIG_X_MAX);
	//gc.position[Y_AXIS]=max(gc.position[Y_AXIS], CONFIG_Y_MIN);
	//gc.position[Y_AXIS]=min(gc.position[Y_AXIS], CONFIG_Y_MAX);

    planner_line( gc.position[X_AXIS] + gc.offsets[3*gc.offselect+X_AXIS],
    				gc.position[Y_AXIS] + gc.offsets[3*gc.offselect+Y_AXIS],
    				gc.position[Z_AXIS] + gc.offsets[3*gc.offselect+Z_AXIS],
    				CONFIG_MAX_SEEKRATE, gc.acceleration, 0 );
}

// Set the offset to the current position.
// Used by the joystick to set 0,0 to to the piece location.
void gcode_set_offset_to_current_position(void) {
	int cs = 0;
    // set offset to current pos, eg: G10 L20 P2
    gc.offsets[3*cs+X_AXIS] = gc.position[X_AXIS] + gc.offsets[3*gc.offselect+X_AXIS];
    gc.offsets[3*cs+Y_AXIS] = gc.position[Y_AXIS] + gc.offsets[3*gc.offselect+Y_AXIS];
    gc.offsets[3*cs+Z_AXIS] = gc.position[Z_AXIS] + gc.offsets[3*gc.offselect+Z_AXIS];

    clear_vector(gc.position);
}

void gcode_do_home(void) {
    stepper_homing_cycle();
    // now that we are at the physical home
    // zero all the position vectors
    clear_vector(gc.position);
    planner_set_position(0.0, 0.0, 0.0);

    // move head to g54 offset
    gc.offselect = OFFSET_G54;
    planner_line( gc.offsets[3*gc.offselect+X_AXIS],
                  gc.offsets[3*gc.offselect+Y_AXIS],
                  gc.offsets[3*gc.offselect+Z_AXIS],
                  gc.seek_rate, gc.acceleration, 0 );
}

// Parses the next statement and leaves the counter on the first character following
// the statement. Returns 1 if there was a statements, 0 if end of string was reached
// or there was an error (check state.status_code).
static int next_statement(char *letter, double *double_ptr, char *line, uint8_t *char_counter) {
  if (line[*char_counter] == 0) {
    return(0); // No more statements
  }
  
  *letter = line[*char_counter];
  if((*letter < 'A') || (*letter > 'Z')) {
    FAIL(STATUS_EXPECTED_COMMAND_LETTER);
    return(0);
  }
  (*char_counter)++;
  if (!read_double(line, char_counter, double_ptr)) {
    FAIL(STATUS_BAD_NUMBER_FORMAT); 
    return(0);
  };
  return(1);
}


// Read a floating point value from a string. Line points to the input buffer, char_counter 
// is the indexer pointing to the current character of the line, while double_ptr is 
// a pointer to the result variable. Returns true when it succeeds
static int read_double(char *line, uint8_t *char_counter, double *double_ptr) {
  char *start = line + *char_counter;
  char *end;
  
  *double_ptr = strtod(start, &end);
  if(end == start) { 
    return(false); 
  };

  *char_counter = end - line;
  return(true);
}






/* 
  Intentionally not supported:

  - arcs {G2, G3}
  - Canned cycles
  - Tool radius compensation
  - A,B,C-axes
  - Evaluation of expressions
  - Variables
  - Multiple home locations
  - Probing
  - Override control

*/
