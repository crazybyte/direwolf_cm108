//
//    This file is part of Dire Wolf, an amateur radio packet TNC.
//
//    Copyright (C) 2014, 2015  John Langner, WB2OSZ
//
//    This program is free software: you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation, either version 2 of the License, or
//    (at your option) any later version.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with this program.  If not, see <http://www.gnu.org/licenses/>.
//


//#define DEBUG1 1		/* Parsing of original human readable format. */
//#define DEBUG2 1		/* Parsing of base 91 compressed format. */
//#define DEBUG3 1		/* Parsing of special messages. */
//#define DEBUG4 1		/* Resulting display form. */

#if TEST

#define DEBUG1 1
#define DEBUG2 1
#define DEBUG3 1
#define DEBUG4 1

#endif


/*------------------------------------------------------------------
 *
 * Module:      telemetry.c
 *
 * Purpose:   	Decode telemetry information.
 *		Point out where it violates the protocol spec and 
 *		other applications might not interpret it properly.
 *		
 * References:	APRS Protocol, chapter 13.
 *		http://www.aprs.org/doc/APRS101.PDF
 *
 *		Base 91 compressed format
 *		http://he.fi/doc/aprs-base91-comment-telemetry.txt
 *
 *---------------------------------------------------------------*/

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

#if __WIN32__
char *strsep(char **stringp, const char *delim);
#endif

#include "direwolf.h"
#include "ax25_pad.h"			// for packet_t, AX25_MAX_ADDR_LEN
#include "decode_aprs.h"		// for decode_aprs_t, G_UNKNOWN  
#include "textcolor.h"
#include "telemetry.h"


#define MAX(x,y) ((x)>(y) ? (x) : (y))


#define T_NUM_ANALOG 5				/* Number of analog channels. */
#define T_NUM_DIGITAL 8				/* Number of digital channnels. */

#define T_STR_LEN 16				/* Max len for labels and units. */


#define MAGIC1  0xa51111a5			/* For checking storage allocation problems. */
#define MAGIC2  0xa52222a5

#define C_A 0					/* Scaling coefficient positions. */
#define C_B 1
#define C_C 2


/*
 * Metadata for telemetry data.
 */

struct t_metadata_s {
	int magic1;

	struct t_metadata_s * pnext;		/* Next in linked list. */

	char station[AX25_MAX_ADDR_LEN];	/* Station name with optional SSID. */

	char project[40];			/* Description for data. */
						/* "Project Name" or "project title" in the spec. */

	char name[T_NUM_ANALOG+T_NUM_DIGITAL][T_STR_LEN];
						/* Names for channels.  e.g. Battery, Temperature */

	char unit[T_NUM_ANALOG+T_NUM_DIGITAL][T_STR_LEN];
						/* Units for channels.  e.g. Volts, Deg.C */

	float coeff[T_NUM_ANALOG][3];		/* a, b, c coefficients for scaling. */

	int coeff_ndp[T_NUM_ANALOG][3];		/* Number of decimal places for above. */

	int sense[T_NUM_DIGITAL];		/* Polarity for digital channels. */

	int magic2;
};


static 	struct t_metadata_s * md_list_head = NULL;

static void t_data_process (struct t_metadata_s *pm, int seq, float araw[T_NUM_ANALOG], int ndp[T_NUM_ANALOG], int draw[T_NUM_DIGITAL], char *output); 


/*-------------------------------------------------------------------
 *
 * Name:        t_get_metadata
 *
 * Purpose:     Obtain pointer to metadata for specified station.
 *		If not found, allocate a fresh one and initialize with defaults.
 *
 * Inputs:	station		- Station name with optional SSID.
 *
 * REturns:	Pointer to metadata.
 *
 *--------------------------------------------------------------------*/

static struct t_metadata_s * t_get_metadata (char *station)
{
	struct t_metadata_s *p;
	int n, j;

#if DEBUG3
	text_color_set(DW_COLOR_DEBUG);
	dw_printf ("t_get_metadata (station=%s)\n", station);
#endif

	for (p = md_list_head; p != NULL; p = p->pnext) {
	  if (strcmp(station, p->station) == 0) {
	    return (p);
	  }
	}

	p = malloc (sizeof (struct t_metadata_s));
	memset (p, 0, sizeof (struct t_metadata_s));

	p->magic1 = MAGIC1;
	strncpy (p->station, station, sizeof(p->station)-1);

	for (n = 0; n < T_NUM_ANALOG; n++) {
	  sprintf (p->name[n], "A%d", n+1);
	}
	for (n = 0; n < T_NUM_DIGITAL; n++) {
	  sprintf (p->name[T_NUM_ANALOG+n], "D%d", n+1);
	}

	for (n = 0; n < T_NUM_ANALOG; n++) {
	  p->coeff[n][C_A] = 0.;
	  p->coeff[n][C_B] = 1.;
	  p->coeff[n][C_C] = 0.;
	  p->coeff_ndp[n][C_A] = 0;
	  p->coeff_ndp[n][C_B] = 0;
	  p->coeff_ndp[n][C_C] = 0;
	}
	
	for (n = 0; n < T_NUM_DIGITAL; n++) {
	  p->sense[n] = 1;
	}

	p->magic2 = MAGIC2;

	p->pnext = md_list_head;
	md_list_head = p;

	assert (p->magic1 == MAGIC1);
	assert (p->magic2 == MAGIC2);

	return (p);

} /* end t_get_metadata */



/*-------------------------------------------------------------------
 *
 * Name:        t_ndp
 *
 * Purpose:     Count number of digits after any decimal point.
 *
 * Inputs:	str	- Number in text format.
 *
 * Returns:	Number digits after decimal point.  Examples, in --> out.
 *
 *			1	--> 0
 *			1.	--> 0
 *			1.2	--> 1
 *			1.23	--> 2
 *			etc.
 *
 *--------------------------------------------------------------------*/

static int t_ndp (char *str)
{
	char *p;

	p = strchr(str,'.');
	if (p == NULL) {
	  return (0);
	}
	else {
	  return (strlen(p+1));
	}
}


/*-------------------------------------------------------------------
 *
 * Name:        telemetry_data_original
 *
 * Purpose:     Interpret telemetry data in the original format.
 *
 * Inputs:	station	- Name of station reporting telemetry.
 *		info 	- Pointer to packet Information field.
 *		quiet	- suppress error messages.
 *
 * Outputs:	output	- Decoded telemetry in human readable format.
 *		comment	- Any comment after the data.
 *
 * Description:	The first character, after the "T" data type indicator, must be "#" 
 *		followed by a sequence number.  Up to 5 analog and 8 digital channel 
 *		values are specified as in this example from the protocol spec.
 *
 *			T#005,199,000,255,073,123,01101001
 *
 *		The analog values are supposed to be 3 digit integers in the 
 *		range of 000 to 255 in fixed columns.  After reading the discussion 
 *		groups it seems that few adhere to those restrictions.  When I 
 *		started to look for some local signals, this was the first one
 *		to appear:
 *
 *			KB1GKN-10>APRX27,UNCAN,WIDE1*:T#491,4.9,0.3,25.0,0.0,1.0,00000000
 *
 *		Not integers.  Not fixed width fields.
 *		We will accept these but issue a warning that others might not.		
 *		
 *--------------------------------------------------------------------*/

void telemetry_data_original (char *station, char *info, int quiet, char *output, char *comment) 
{
	int n;
	int seq;
	char stemp[256];
	char *next;
	char *p;

	float araw[T_NUM_ANALOG];
	int ndp[T_NUM_ANALOG];
	int draw[T_NUM_DIGITAL];

	struct t_metadata_s *pm;


#if DEBUG1
	text_color_set(DW_COLOR_DEBUG);

	dw_printf ("\n%s\n\n", info);
#endif

	pm = t_get_metadata(station);
	seq = 0;
	for (n = 0; n < T_NUM_ANALOG; n++) {
	  araw[n] = G_UNKNOWN;
	  ndp[n] = 0;
	}
	for (n = 0; n < T_NUM_DIGITAL; n++) {
	  draw[n] = G_UNKNOWN;
	}

	if (strncmp(info, "T#", 2) != 0) {
	  if ( ! quiet) {
	    text_color_set(DW_COLOR_ERROR);
	    dw_printf("Error: Information part of telemetry packet must begin with \"#\"\n");
	  }
	  return;	
	}

/*
 * Make a copy of the input string because this will alter it.
 */

	strncpy (stemp, info+2, sizeof(stemp));
	stemp[sizeof(stemp)-1] = '\0';

	next = stemp;
	p = strsep(&next,",");
	seq = atoi(p);
	n = 0;
	while ((p = strsep(&next,",")) != NULL) {
	  if (n < T_NUM_ANALOG) {
	    if (strlen(p) > 0) {
	      araw[n] = atof(p);
	      ndp[n] = t_ndp(p);
	    }
	    if (strlen(p) != 3 || araw[n] < 0 || araw[n] > 255 || araw[n] != (int)(araw[n])) {
	      if ( ! quiet) {
	        text_color_set(DW_COLOR_ERROR);
	        dw_printf("Telemetry analog values should be 3 digit integer values in range of 000 to 255.\n");	      
	        dw_printf("Some applications might not interpret \"%s\" properly.\n", p);
	      }	      
	    }
	    n++;
	  }

	  if (n == T_NUM_ANALOG && next != NULL) {
	    /* We expect to have 8 digits of 0 and 1. */
	    /* Anything left over is a comment. */

	    int k;

	    if (strlen(next) < 8) {
	      if ( ! quiet) {
	        text_color_set(DW_COLOR_ERROR);
	        dw_printf("Expected to find 8 binary digits after \"%s\" for the digital values.\n", p);	
	      }      
	    }
	    if (strlen(next) > 8) {
	      strcpy (comment, next+8);
	      next[8] = '\0';
	    }
	    for (k = 0; k < strlen(next); k++) {
	      if (next[k] == '0') {
	        draw[k] = 0;
	      }
	      else if (next[k] == '1') {
	        draw[k] = 1;
	      }
	      else {
	        if ( ! quiet) {
	          text_color_set(DW_COLOR_ERROR);
	          dw_printf("Found \"%c\" when expecting 0 or 1 for digital value %d.\n", next[k], k+1);	
	        }      
	      }
 	    }
	    n++;
	  }
	}
	if (n < T_NUM_ANALOG+1) {
	  if ( ! quiet) {
	    text_color_set(DW_COLOR_ERROR);
	    dw_printf("Found fewer than expected number of telemetry data values.\n");	
	  }
	}      

/*
 * Now process the raw data with any metadata available.
 */
	
#if DEBUG1
	text_color_set(DW_COLOR_DECODED);

	dw_printf ("%d: %.3f %.3f %.3f %.3f %.3f \n", 
		seq, araw[0], araw[1], araw[2], araw[3], araw[4]);

	dw_printf ("%d %d %d %d %d %d %d %d \"%s\"\n",
		draw[0], draw[1], draw[2], draw[3], draw[4], draw[5], draw[6], draw[7], comment);

#endif

	t_data_process (pm, seq, araw, ndp, draw, output);

} /* end telemtry_data_original */


/*-------------------------------------------------------------------
 *
 * Name:        telemetry_data_base91
 *
 * Purpose:     Interpret telemetry data in the base 91 compressed format.
 *
 * Inputs:	station	- Name of station reporting telemetry.
 *		cdata 	- Compressed data as character string.
 *
 * Outputs:	output	- Telemetry in human readable form.
 *
 * Description:	We are expecting from 2 to 7 pairs of base 91 digits.
 *		The first pair is the sequence number.
 *		Next we have 1 to 5 analog values.
 *		If digital values are present, all 5 analog values must be present.	
 *		
 *--------------------------------------------------------------------*/

/* Range of digits for Base 91 representation. */

#define B91_MIN '!'
#define B91_MAX '{'
#define isdigit91(c) ((c) >= B91_MIN && (c) <= B91_MAX)


static int two_base91_to_i (char *c)
{
	int result = 0;

	assert (B91_MAX - B91_MIN == 90);

	if (isdigit91(c[0])) {
	  result = (c[0] - B91_MIN) * 91;
	}
	else {
	  text_color_set(DW_COLOR_DEBUG);
	  dw_printf ("\"%c\" is not a valid character for base 91 telemetry data.\n", c[0]);
	}

	if (isdigit91(c[1])) {
	  result += (c[1] - B91_MIN);
	}
	else {
	  text_color_set(DW_COLOR_DEBUG);
	  dw_printf ("\"%c\" is not a valid character for base 91 telemetry data.\n", c[1]);
	}
	return (result);
}

void telemetry_data_base91 (char *station, char *cdata, char *output)
{
	int n;
	int seq;
	char *p;

	float araw[T_NUM_ANALOG];
	int ndp[T_NUM_ANALOG];
	int draw[T_NUM_DIGITAL];
	struct t_metadata_s *pm;

#if DEBUG2
	text_color_set(DW_COLOR_DEBUG);

	dw_printf ("\n%s\n\n", cdata);
#endif

	pm = t_get_metadata(station);

	seq = 0;
	for (n = 0; n < T_NUM_ANALOG; n++) {
	  araw[n] = G_UNKNOWN;
	  ndp[n] = 0;
	}
	for (n = 0; n < T_NUM_DIGITAL; n++) {
	  draw[n] = G_UNKNOWN;
	}

	if (strlen(cdata) < 4 || strlen(cdata) > 14 || (strlen(cdata) & 1)) {
	  text_color_set(DW_COLOR_ERROR);
	  dw_printf("Internal error: Expected even number of 2 to 14 characters but got \"%s\"\n", cdata);
	  return;	
	}

	seq = two_base91_to_i (cdata);

	for (n=0, p=cdata+2; n<T_NUM_ANALOG+1 && *p!='\0'; n++,p+=2) {
	  if (n < T_NUM_ANALOG) {
	    araw[n] = two_base91_to_i (p);
	  }
	  else {
	    int k;
	    int b = two_base91_to_i (p);
	    for (k=0; k<T_NUM_DIGITAL; k++) {
	      draw[k] = b & 1;
	      b >>= 1;
	    }
	  }
	}

/*
 * Now process the raw data with any metadata available.
 */
	
#if DEBUG2
	text_color_set(DW_COLOR_DECODED);

	dw_printf ("%d: %.3f %.3f %.3f %.3f %.3f \n", 
		seq, araw[0], araw[1], araw[2], araw[3], araw[4]);

	dw_printf ("%d %d %d %d %d %d %d %d \n",
		draw[0], draw[1], draw[2], draw[3], draw[4], draw[5], draw[6], draw[7]);

#endif

	t_data_process (pm, seq, araw, ndp, draw, output);

} /* end telemtry_data_base91 */



/*-------------------------------------------------------------------
 *
 * Name:        telemetry_name_message
 *
 * Purpose:     Interpret message with names for analog and digital channels.
 *
 * Inputs:	station	- Name of station reporting telemetry.
 *			  In this case it is the destination for the message,
 *			  not the sender.
 *		msg 	- Rest of message after "PARM."
 *
 * Outputs:	Stored for future use when data values are received.
 *
 * Description:	The first 5 characters of the message are "PARM." and the
 *		rest is a variable length list of comma separated names.
 *	
 *		The original spec has different maximum lengths for different
 *		fields which we will ignore.
 *
 * TBD:		What should we do if some, but not all, names are specified?
 *		Clear the others or keep the defaults?
 *	
 *--------------------------------------------------------------------*/

void telemetry_name_message (char *station, char *msg) 
{
	int n;
	char stemp[256];
	char *next;
	char *p;
	struct t_metadata_s *pm;

#if DEBUG3
	text_color_set(DW_COLOR_DEBUG);

	dw_printf ("\n%s\n\n", msg);
#endif


/*
 * Make a copy of the input string because this will alter it.
 */

	strncpy (stemp, msg, sizeof(stemp));
	stemp[sizeof(stemp)-1] = '\0';

	pm = t_get_metadata(station);

	next = stemp;

	n = 0;
	while ((p = strsep(&next,",")) != NULL) {
	  if (n < T_NUM_ANALOG + T_NUM_DIGITAL) {
	    if (strlen(p) > 0 && strcmp(p,"-") != 0) {
	      strncpy (pm->name[n], p, T_STR_LEN-1);
	    }
	    n++;
	  }
	}

#if DEBUG3
	text_color_set(DW_COLOR_DEBUG);

	dw_printf ("names:\n");
	for (n = 0; n < T_NUM_ANALOG + T_NUM_DIGITAL; n++) {
	  dw_printf ("%d=\"%s\"\n", n, pm->name[n]);
	}
#endif

} /* end telemetry_name_message */



/*-------------------------------------------------------------------
 *
 * Name:        telemetry_unit_label_message
 *
 * Purpose:     Interpret message with units/labels for analog and digital channels.
 *
 * Inputs:	station	- Name of station reporting telemetry.
 *			  In this case it is the destination for the message,
 *			  not the sender.
 *		msg 	- Rest of message after "UNIT."
 *
 * Outputs:	Stored for future use when data values are received.
 *
 * Description:	The first 5 characters of the message are "UNIT." and the
 *		rest is a variable length list of comma separated units/labels.
 *	
 *		The original spec has different maximum lengths for different
 *		fields which we will ignore.
 *
 *--------------------------------------------------------------------*/

void telemetry_unit_label_message (char *station, char *msg) 
{
	int n;
	char stemp[256];
	char *next;
	char *p;
	struct t_metadata_s *pm;

#if DEBUG3
	text_color_set(DW_COLOR_DEBUG);

	dw_printf ("\n%s\n\n", msg);
#endif


/*
 * Make a copy of the input string because this will alter it.
 */

	strncpy (stemp, msg, sizeof(stemp));
	stemp[sizeof(stemp)-1] = '\0';

	pm = t_get_metadata(station);

	next = stemp;

	n = 0;
	while ((p = strsep(&next,",")) != NULL) {
	  if (n < T_NUM_ANALOG + T_NUM_DIGITAL) {
	    if (strlen(p) > 0) {
	      strncpy (pm->unit[n], p, T_STR_LEN-1);
	    }
	    n++;
	  }
	}

#if DEBUG3
	text_color_set(DW_COLOR_DEBUG);

	dw_printf ("units/labels:\n");
	for (n = 0; n < T_NUM_ANALOG + T_NUM_DIGITAL; n++) {
	  dw_printf ("%d=\"%s\"\n", n, pm->unit[n]);
	}
#endif

} /* end telemetry_unit_label_message */



/*-------------------------------------------------------------------
 *
 * Name:        telemetry_coefficents_message
 *
 * Purpose:     Interpret message with scaling coefficents for analog channels.
 *
 * Inputs:	station	- Name of station reporting telemetry.
 *			  In this case it is the destination for the message,
 *			  not the sender.
 *		msg 	- Rest of message after "EQNS."
 *		quiet	- suppress error messages.
 *
 * Outputs:	Stored for future use when data values are received.
 *
 * Description:	The first 5 characters of the message are "EQNS." and the
 *		rest is a comma separated list of 15 floating point values.
 *	
 *		The spec appears to require all 15 so we will issue an
 *		error if fewer found.
 *
 *--------------------------------------------------------------------*/

void telemetry_coefficents_message (char *station, char *msg, int quiet) 
{
	int n;
	char stemp[256];
	char *next;
	char *p;
	struct t_metadata_s *pm;

#if DEBUG3
	text_color_set(DW_COLOR_DEBUG);

	dw_printf ("\n%s\n\n", msg);
#endif


/*
 * Make a copy of the input string because this will alter it.
 */

	strncpy (stemp, msg, sizeof(stemp));
	stemp[sizeof(stemp)-1] = '\0';

	pm = t_get_metadata(station);

	next = stemp;

	n = 0;
	while ((p = strsep(&next,",")) != NULL) {
	  if (n < T_NUM_ANALOG * 3) {
	    // Keep default (or earlier value) for an empty field.
	    if (strlen(p) > 0) {
	      pm->coeff[n/3][n%3] = atof (p);
	      pm->coeff_ndp[n/3][n%3] = t_ndp (p);
	    }
	    else {
	      if ( ! quiet) {
	        text_color_set(DW_COLOR_ERROR);
	        dw_printf ("Equation coefficent position A%d%c is empty.\n", n/3+1, n%3+'a');
	        dw_printf ("Some applications might not handle this correctly.\n");
	      }
	    }
	  }
	  n++;
	}

	if (n != T_NUM_ANALOG * 3) {
	  if ( ! quiet) {
	    text_color_set(DW_COLOR_ERROR);
	    dw_printf ("Found %d equation coefficents when 15 were expected.\n", n);
	    dw_printf ("Some applications might not handle this correctly.\n");
	  }
	}

#if DEBUG3
	text_color_set(DW_COLOR_DEBUG);

	dw_printf ("coeff:\n");
	for (n = 0; n < T_NUM_ANALOG; n++) {
	  dw_printf ("A%d  a=%.*f  b=%.*f  c=%.*f\n", n+1, 
			pm->coeff_ndp[n][C_A], pm->coeff[n][C_A], 
			pm->coeff_ndp[n][C_B], pm->coeff[n][C_B], 
			pm->coeff_ndp[n][C_C], pm->coeff[n][C_C]);
	}
#endif

} /* end telemetry_coefficents_message */



/*-------------------------------------------------------------------
 *
 * Name:        telemetry_bit_sense_message
 *
 * Purpose:     Interpret message with scaling coefficents for analog channels.
 *
 * Inputs:	station	- Name of station reporting telemetry.
 *			  In this case it is the destination for the message,
 *			  not the sender.
 *		msg 	- Rest of message after "BITS."
 *		quiet	- suppress error messages.
 *
 * Outputs:	Stored for future use when data values are received.
 *
 * Description:	The first 5 characters of the message are "BITS."
 *		It should contain eight binary digits for the digital active states.
 *		Anything left over is the project name or title.
 *
 *--------------------------------------------------------------------*/

void telemetry_bit_sense_message (char *station, char *msg, int quiet) 
{
	int n;
	struct t_metadata_s *pm;

#if DEBUG3
	text_color_set(DW_COLOR_DEBUG);

	dw_printf ("\n%s\n\n", msg);
#endif

	pm = t_get_metadata(station);

	if (strlen(msg) < 8) {
	  if ( ! quiet) {
	    text_color_set(DW_COLOR_ERROR);
	    dw_printf ("The telemetry bit sense message should have at least 8 characters.\n");
	  }
	}

	for (n = 0; n < T_NUM_DIGITAL && n < strlen(msg); n++) {

	  if (msg[n] == '1') {
	    pm->sense[n] = 1;
	  }
	  else if (msg[n] == '0') {
	    pm->sense[n] = 0;
	  }
	  else {
	    if ( ! quiet) {
	      text_color_set(DW_COLOR_ERROR);
	      dw_printf ("Bit position %d sense value was \"%c\" when 0 or 1 was expected.\n", n+1, msg[n]);
	    }
	  }
	}

/* Skip comma if first character of comment field. */

	if (msg[n] == ',') n++;

	strncpy (pm->project, msg+n, sizeof(pm->project)-1);
 
#if DEBUG3
	text_color_set(DW_COLOR_DEBUG);

	dw_printf ("bit sense, project:\n");
	dw_printf ("%d %d %d %d %d %d %d %d \"%s\"\n", 
		pm->sense[0],
		pm->sense[1],
		pm->sense[2],
		pm->sense[3],
		pm->sense[4],
		pm->sense[5],
		pm->sense[6],
		pm->sense[7],
		pm->project);


#endif

} /* end telemetry_bit_sense_message */


/*-------------------------------------------------------------------
 *
 * Name:        t_data_process
 *
 * Purpose:     Interpret telemetry data in the original format.
 *
 * Inputs:	pm	- Pointer to metadata.
 *		seq	- Sequence number.
 *		araw	- 5 analog raw values.
 *		ndp	- Number of decimal points for each.
 *		draw	- 8 digial raw vales.
 *
 * Outputs:	output	- Decoded telemetry in human readable format.
 *
 * Description:	Process raw data according to any metadata available
 *		and put into human readable form.	
 *		
 *--------------------------------------------------------------------*/

static void fval_to_str (float x, int ndp, char *str)
{
	if (x == G_UNKNOWN) {
	  strcpy (str, "?");
	}
	else {
	  sprintf (str, "%.*f", ndp, x);
	}
}

static void ival_to_str (int x, char *str)
{
	if (x == G_UNKNOWN) {
	  strcpy (str, "?");
	}
	else {
	  sprintf (str, "%d", x);
	}
}

static void t_data_process (struct t_metadata_s *pm, int seq, float araw[T_NUM_ANALOG], int ndp[T_NUM_ANALOG], int draw[T_NUM_DIGITAL], char *output) 
{
	int n;
	char stemp[50];


	assert (pm != NULL);
	assert (pm->magic1 == MAGIC1);
	assert (pm->magic2 == MAGIC2);

	strcpy (output, "");

	if (strlen(pm->project) > 0) {
	  strcpy (output, pm->project);
	  strcat (output, ": ");
	}

	ival_to_str (seq, stemp);
	strcat (output, "Seq=");
	strcat (output, stemp);
	
	for (n = 0; n < T_NUM_ANALOG; n++) {
	  
	  // Display all or only defined values?  Only defined for now.

	  if (araw[n] != G_UNKNOWN) {
	    float fval;
	    int fndp;

	    strcat (output, ", ");	

	    strcat (output, pm->name[n]);
	    strcat (output, "=");
	    
	    // Scaling and suitable number of decimal places for display.

	    if (araw[n] == G_UNKNOWN) {
	      fval = G_UNKNOWN;
	      fndp = 0;
	    }
	    else {
	      int z;

	      fval = pm->coeff[n][C_A] * araw[n] * araw[n] +
		     pm->coeff[n][C_B] * araw[n] +
		     pm->coeff[n][C_C];
	      
	      z = pm->coeff_ndp[n][C_A] == 0 ? 0 : pm->coeff_ndp[n][C_A] + ndp[n] + ndp[n];
	      fndp = MAX (z, MAX(pm->coeff_ndp[n][C_B] + ndp[n], pm->coeff_ndp[n][C_C]));
	    }
	    fval_to_str (fval, fndp, stemp);
	    strcat (output, stemp);
	    if (strlen(pm->unit[n]) > 0) {
	      strcat (output, " ");
	      strcat (output, pm->unit[n]);
	    }
	    
	  }
	}

	for (n = 0; n < T_NUM_DIGITAL; n++) {
	  
	  // Display all or only defined values?  Only defined for now.

	  if (draw[n] != G_UNKNOWN) {
	    int dval;

	    strcat (output, ", ");	

	    strcat (output, pm->name[T_NUM_ANALOG+n]);
	    strcat (output, "=");
	    
	    // Possible inverting for bit sense.

	    if (draw[n] == G_UNKNOWN) {
	      dval = G_UNKNOWN;
	    }
	    else {
	      dval = draw[n] ^ ! pm->sense[n];
	    }

	    ival_to_str (dval , stemp);

	    if (strlen(pm->unit[T_NUM_ANALOG+n]) > 0) {
	      strcat (output, " ");
	      strcat (output, pm->unit[T_NUM_ANALOG+n]);
	    }
	    strcat (output, stemp);
	    
	  }
	}


#if DEBUG4
	text_color_set(DW_COLOR_DEBUG);

	dw_printf ("%s\n", output);	
#endif

} /* end t_data_process */


/*-------------------------------------------------------------------
 *
 * Unit test.   Run with:
 *
 *	make -f Makefile.? etest
 *
 *--------------------------------------------------------------------*/


#if TEST



int main ( )
{
	char result[256];
	char comment[256];

	strcpy (result, "");
	strcpy (comment, "");


	text_color_set(DW_COLOR_INFO);
	dw_printf ("Unit test for telemetry decoding functions...\n");	

#if DEBUG1

	text_color_set(DW_COLOR_INFO);
	dw_printf ("part 1\n");	

	// From protocol spec.

	telemetry_data_original ("WB2OSZ", "T#005,199,000,255,073,123,01101001", 0, result, comment);

	// Try adding a comment.

	telemetry_data_original ("WB2OSZ", "T#005,199,000,255,073,123,01101001Comment,with,commas", 0, result, comment);
	strcpy (comment, "");

	// Try shortening or omitting parts.

	telemetry_data_original ("WB2OSZ", "T005,199,000,255,073,123,0110", 0, result, comment);
	telemetry_data_original ("WB2OSZ", "T#005,199,000,255,073,123,0110", 0, result, comment);
	telemetry_data_original ("WB2OSZ", "T#005,199,000,255,073,123", 0, result, comment);
	telemetry_data_original ("WB2OSZ", "T#005,199,000,255,,123,01101001", 0, result, comment);
	telemetry_data_original ("WB2OSZ", "T#005,199,000,255,073,123,01101009", 0, result, comment);

	// Local observation.

	telemetry_data_original ("WB2OSZ", "T#491,4.9,0.3,25.0,0.0,1.0,00000000", 0, result, comment); 

#endif

#if DEBUG2

	text_color_set(DW_COLOR_INFO);
	dw_printf ("part 2\n");	

	// From protocol spec.

	telemetry_data_base91 ("WB2OSZ", "ss11", result);
	dw_printf ("expect 7544: 1472 above.\n");

	telemetry_data_base91 ("WB2OSZ", "ss11223344{{!\"", result);
	dw_printf ("expect 7544: 1472, 1564, 1656, 1748, 8280, 10000000 above.\n");

	// Error cases.  Should not happen in practice because function
	// should be called only with valid data that matches the pattern.

	telemetry_data_base91 ("WB2OSZ", "ss11223344{{!\"x", result);
	telemetry_data_base91 ("WB2OSZ", "ss1", result);
	telemetry_data_base91 ("WB2OSZ", "ss11223344{{!", result);
	telemetry_data_base91 ("WB2OSZ", "s |1", result);

#endif

#if DEBUG3

	text_color_set(DW_COLOR_INFO);
	dw_printf ("part 3\n");	

	telemetry_name_message ("N0QBF-11", "Battery,Btemp,ATemp,Pres,Alt,Camra,Chut,Sun,10m,ATV");

	telemetry_unit_label_message ("N0QBF-11", "v/100,deg.F,deg.F,Mbar,Kft,Click,OPEN,on,on,hi");

	telemetry_coefficents_message ("N0QBF-11", "0,5.2,0,0,.53,-32,3,4.39,49,-32,3,18,1,2,3", 0);

	// Error if less than 15 or empty field.
	telemetry_coefficents_message ("N0QBF-11", "0,5.2,0,0,.53,-32,3,4.39,49,-32,3,18,1,2", 0);
	telemetry_coefficents_message ("N0QBF-11", "0,5.2,0,0,.53,-32,3,4.39,49,-32,3,18,1,,3", 0);

	telemetry_bit_sense_message ("N0QBF-11", "10110000,N0QBF's Big Balloon", 0);

	// Too few and invalid digits.
	telemetry_bit_sense_message ("N0QBF-11", "1011000", 0);
	telemetry_bit_sense_message ("N0QBF-11", "10110008", 0);

#endif

	text_color_set(DW_COLOR_INFO);
	dw_printf ("part 4\n");	

	telemetry_coefficents_message ("M0XER-3", "0,0.001,0,0,0.001,0,0,0.1,-273.2,0,1,0,0,1,0", 0);
	telemetry_bit_sense_message ("M0XER-3", "11111111,10mW research balloon", 0);
	telemetry_name_message ("M0XER-3", "Vbat,Vsolar,Temp,Sat");
	telemetry_unit_label_message ("M0XER-3", "V,V,C,,m");

	telemetry_data_base91 ("M0XER-3", "DyR.&^<A!.", result);
	telemetry_data_base91 ("M0XER-3", "cNOv'C?=!-", result);
	telemetry_data_base91 ("M0XER-3", "n0RS(:>b!+", result);
	telemetry_data_base91 ("M0XER-3", "x&G=!(8s!,", result);


	// TODO: Should return success/fail so visual inspection is not needed. 

	exit (0);
}

/*
	A more complete test can be performed by placing the following
	in a text file and feeding it into the "decode_aprs" utility.

2E0TOY>APRS::M0XER-3  :BITS.11111111,10mW research balloon
2E0TOY>APRS::M0XER-3  :PARM.Vbat,Vsolar,Temp,Sat
2E0TOY>APRS::M0XER-3  :EQNS.0,0.001,0,0,0.001,0,0,0.1,-273.2,0,1,0,0,1,0
2E0TOY>APRS::M0XER-3  :UNIT.V,V,C,,m
M0XER-3>APRS63,WIDE2-1:!//Bap'.ZGO JHAE/A=042496|E@Q0%i;5!-|
M0XER-3>APRS63,WIDE2-1:!/4\;u/)K$O J]YD/A=041216|h`RY(1>q!(|
M0XER-3>APRS63,WIDE2-1:!/23*f/R$UO Jf'x/A=041600|rxR_'J>+!(|

	The interpretation should look something like this:
	10mW research balloon: Seq=3307, Vbat=4.383 V, Vsolar=0.436 V, Temp=-34.6 C, Sat=12
*/

#endif

/* end telemetry.c */
