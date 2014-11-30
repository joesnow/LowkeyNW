/*
** record_plus~.c
**
** MSP object
** signal and delta checking for record control
** starts & stops only after zero crossing occurs
** 
** 2004/08/05 started
** 2004/08/07 working
** 
*/

#include "ext.h"		// required for all MAX external objects
#include "z_dsp.h"		// required for all MSP external objects
#include "buffer.h"		// required to deal with buffer object
#include "ext_systime.h"// required to get ticks
#include <string.h>

//#define DEBUG			//enable debugging messages

#define OBJECT_NAME		"record_plus~"		// name of the object

/* for the assist method */
#define ASSIST_INLET	1
#define ASSIST_OUTLET	2

/* for interpolation flag */
#define INTERP_OFF			0
#define INTERP_ON			1

/* for record stage flag */
#define REC_OFF			0
#define MONITOR_ON		1
#define REC_ON			2
#define MONITOR_OFF		3

void *this_class;		// required global pointing to this class

typedef struct _recordplus
{
	t_pxobject x_obj;					// <--
	
	// sound buffer info
	t_symbol *snd_sym;
	t_buffer *snd_buf_ptr;
	t_symbol *next_snd_sym;
	t_buffer *next_snd_buf_ptr;
	
	// current recording info
	long rec_position;	// in samples
	short rec_stage; 	// see flags in header
	
	// sync output info
	double sync_val;	// output from sync outlet
	double sync_step;	// amount to add each time a sample is recorded
	double next_sync_step;	// changes when new buffer is set
	
	// history
	double last_ctrl_in;
	double last_sig_in;
	
	double input_sr;					// <--
	double input_1oversr;				// <--
	double input_msr;					// <--
	
} t_recordplus;

void *recordplus_new(t_symbol *snd);
t_int *recordplus_perform(t_int *w);
t_int *recordplus_perform0(t_int *w);
void recordplus_dsp(t_recordplus *x, t_signal **sp, short *count);
void recordplus_setbuff(t_recordplus *x, t_symbol *s);
short recordplus_updatebuff(t_recordplus *x);
void recordplus_assist(t_recordplus *x, t_object *b, long msg, long arg, char *s);
void recordplus_getinfo(t_recordplus *x);

t_symbol *ps_buffer;

/********************************************************************************
void main(void)

inputs:			nothing
description:	called the first time the object is used in MAX environment; 
		defines inlets, outlets and accepted messages
returns:		nothing
********************************************************************************/
void main(void)
{
	setup((t_messlist **)&this_class, (method)recordplus_new, (method)dsp_free, 
			(short)sizeof(t_recordplus), 0L, A_SYM, 0);
	addmess((method)recordplus_dsp, "dsp", A_CANT, 0);
	
	/* bind method "recordplus_setbuff" to the 'set' message */
	addmess((method)recordplus_setbuff, "set", A_SYM, 0);
	
	/* bind method "recordplus_assist" to the assistance message */
	addmess((method)recordplus_assist, "assist", A_CANT, 0);
	
	/* bind method "recordplus_getinfo" to the getinfo message */
	addmess((method)recordplus_getinfo, "getinfo", A_NOTHING, 0);
	
	dsp_initclass();
	
	/* needed for 'buffer~' work, checks for validity of buffer specified */
	ps_buffer = gensym("buffer~");
	
	#ifndef DEBUG
		
	#endif /* DEBUG */
}

/********************************************************************************
void *recordplus_new(double initial_pos)

inputs:			*snd		-- name of buffer to record to
description:	called for each new instance of object in the MAX environment;
		defines inlets and outlets; sets variables and buffers
returns:		nothing
********************************************************************************/
void *recordplus_new(t_symbol *snd)
{
	t_recordplus *x = (t_recordplus *)newobject(this_class);
	dsp_setup((t_pxobject *)x, 2);					// two inlets
	outlet_new((t_pxobject *)x, "signal");			// sync outlet
	
	/* set buffer names */
	x->snd_sym = snd;
	
	/* zero pointers */
	x->snd_buf_ptr = x->next_snd_buf_ptr = NULL;
	
	/* setup variables */
	x->rec_position = 0;
	x->sync_val = 0.0;
	x->sync_step = 0.0;
	x->last_ctrl_in = 0.0;
	x->last_sig_in = 0.0;
	
	/* set flags to defaults */
	x->rec_stage = REC_OFF;
	
	x->x_obj.z_misc = Z_NO_INPLACE;
	
	/* return a pointer to the new object */
	return (x);
}

/********************************************************************************
void recordplus_dsp(t_cpPan *x, t_signal **sp, short *count)

inputs:			x		-- pointer to this object
				sp		-- array of pointers to input & output signals
				count	-- array of shorts detailing number of signals attached
					to each inlet
description:	called when DSP call chain is built; adds object to signal flow
returns:		nothing
********************************************************************************/
void recordplus_dsp(t_recordplus *x, t_signal **sp, short *count)
{
	/* set buffers */
	recordplus_setbuff(x, x->snd_sym);
	
	x->input_sr = sp[1]->s_sr;
	x->input_1oversr = 1.0 / x->input_sr;
	x->input_msr = x->input_sr * 0.001;
	
	if (count[1] && count[0]) {	// if inputs connected..
		// output is computed
		dsp_add(recordplus_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, 
			sp[1]->s_n);
		#ifdef DEBUG
			post("%s: output is being computed", OBJECT_NAME);
		#endif /* DEBUG */
	} else {					// if not...
		// no output computed
		//dsp_add(recordplus_perform0, 2, sp[4]->s_vec, sp[4]->s_n);
		#ifdef DEBUG
			post("%s: no output computed", OBJECT_NAME);
		#endif /* DEBUG */
	}
	
}

/********************************************************************************
t_int *recordplus_perform(t_int *w)

inputs:			w		-- array of signal vectors specified in "recordplus_dsp"
description:	called at interrupt level to compute object's output; used when
		outlets are connected; tests inlet 2 3 & 4 to use either control or audio
		rate data
returns:		pointer to the next 
********************************************************************************/
t_int *recordplus_perform(t_int *w)
{
	t_recordplus *x = (t_recordplus *)(w[1]);
	float *in_ctrl = (float *)(w[2]);
	float *in_signal = (float *)(w[3]);
	t_float *out = (t_float *)(w[4]);
	int vec_size = (int)(w[5]);
	t_buffer *s_ptr = x->snd_buf_ptr;
	short r_stage;
	double lc_in, ls_in, sync_v, sync_s;
	long r_pos, s_size;
	float *s_tab;
	long saveinuse, saverpos;
	
	vec_size += 1;		//increase by one for pre-decrement
	--out;				//decrease by one for pre-increment
	
	/* check to make sure buffers are loaded with proper file types*/
	if (x->x_obj.z_disabled)						// object is enabled
		goto out;
	if (s_ptr == NULL)		// buffer pointers are defined
		goto zero;
	if (!s_ptr->b_valid)		// files are loaded
		goto zero;
	
	// set "in use" to true
	saveinuse = s_ptr->b_inuse;
	s_ptr->b_inuse = true;
	
	// assign values to local vars
	r_stage = x->rec_stage;
	lc_in = x->last_ctrl_in;
	ls_in = x->last_sig_in;
	sync_v = x->sync_val;
	sync_s = x->sync_step;
	r_pos = x->rec_position;
	s_size = s_ptr->b_frames;
	s_tab = s_ptr->b_samples;
	
	// track r_pos to see if we wrote anything
	saverpos = r_pos;
	
	while (--vec_size) {
		
		// test ctrl input
		if ((lc_in == 0.) != (*in_ctrl == 0.)) // change in control
		{
			switch (r_stage)
			{
				case REC_OFF:
					++r_stage; // MONITOR_ON
					if (recordplus_updatebuff(x)) 
					{
						s_ptr = x->snd_buf_ptr;
						s_tab = s_ptr->b_samples;
						s_size = s_ptr->b_frames;
						sync_v = x->sync_val;
						sync_s = x->sync_step;
						r_pos = x->rec_position;
					}
					break;
				case MONITOR_ON:
					--r_stage; // REC_OFF
					break;
				case REC_ON:
					++r_stage; // MONITOR_OFF
					break;
				case MONITOR_OFF:
					--r_stage; // REC_ON
					break;
			}
		}
		
		// test for positive zero-crossing
		if (r_stage % 2) // if MONITOR_ON or MONITOR_OFF
		{
			if (ls_in < 0. && *in_signal >= 0.)
			{
				switch (r_stage)
				{
					case MONITOR_ON:
						++r_stage; // REC_ON
						break;
					case MONITOR_OFF:
						r_stage = REC_OFF;
						break;
				}
			}
		}
		
		// record under right conditions
		if (r_stage > MONITOR_ON) // REC_ON or MONITOR_OFF
		{
			
			s_tab[r_pos] = *in_signal;
			
			++r_pos;
			while (r_pos > s_size) 
			{
				r_pos = 0;
			}
			
			sync_v += sync_s;
			while (sync_v > 1.0) 
			{
				sync_v = 0.0;
			}
		}
		
		// update history
		lc_in = *in_ctrl;
		ls_in = *in_signal;
		
		// advance other pointers
		++in_ctrl, ++in_signal;
		
		// update sync output
		*++out = sync_v;
		
	}
	
	// update modtime
	if (r_pos > saverpos)
		s_ptr->b_modtime = systime_ticks();
	
	// update global vars
	x->rec_stage = r_stage;
	x->last_ctrl_in = lc_in;
	x->last_sig_in = ls_in;
	x->sync_val = sync_v;
	x->rec_position = r_pos;
	
	// reset "in use"
	s_ptr->b_inuse = saveinuse;
	
	return (w + 6);

zero:
		while (--vec_size) {
			*++out = 0.0;
		}
out:
		return (w + 6);
}	

/********************************************************************************
t_int *recordplus_perform0(t_int *w)

inputs:			w		-- array of signal vectors specified in "recordplus_dsp"
description:	called at interrupt level to compute object's output; used when
		nothing is connected to output; saves CPU cycles
returns:		pointer to the next 
********************************************************************************/
t_int *recordplus_perform0(t_int *w)
{
	t_float *out = (t_float *)(w[1]);
	int vec_size = (int)(w[2]);

	vec_size += 1;		//increase by one for pre-decrement
	--out;				//decrease by one for pre-increment

	while (--vec_size >= 0) {
		*++out = 0.;
	}

	return (w + 3);
}

/********************************************************************************
void recordplus_setbuff(t_recordplus *x, t_symbol *s)

inputs:			x		-- pointer to this object
				s		-- name of buffer to link
description:	links buffer for recording 
returns:		nothing
********************************************************************************/
void recordplus_setbuff(t_recordplus *x, t_symbol *s)
{
	t_buffer *b;
	
	if ((b = (t_buffer *)(s->s_thing)) && ob_sym(b) == ps_buffer) {
		if (b->b_nchans != 1) {
			error("%s: buffer~ > %s < must be mono", OBJECT_NAME, s->s_name);
			x->next_snd_buf_ptr = NULL;
		} else {
			if (x->snd_buf_ptr == NULL) { // if first buffer make current buffer
				x->snd_sym = s;
				x->sync_val = 0.0;
				x->sync_step = 1.0 / (float)b->b_frames;
				x->rec_position = 0;
				x->snd_buf_ptr = b;			// last so that all is ready
				
				#ifdef DEBUG
					post("%s: current sound set to buffer~ > %s <", OBJECT_NAME, s->s_name);
				#endif /* DEBUG */
			} else { // else defer to next buffer
				if (b != x->snd_buf_ptr) // if it is not the same as current
				{
					x->next_snd_sym = s;
					x->next_sync_step = 1.0 / (float)b->b_frames;
					x->next_snd_buf_ptr = b;	// last so that all is ready
					
					#ifdef DEBUG
						post("%s: next sound set to buffer~ > %s <", OBJECT_NAME, s->s_name);
					#endif /* DEBUG */
				}
			}
		}
	} else {
		error("%s: no buffer~ * %s * found", OBJECT_NAME, s->s_name);
		x->next_snd_buf_ptr = NULL;
	}
}

/********************************************************************************
short recordplus_updatebuff(t_recordplus *x)

inputs:			x		-- pointer to this object
description:	updates if deferred buffer has been specified 
returns:		0 or 1 (false or true)
********************************************************************************/
short recordplus_updatebuff(t_recordplus *x)
{
	if (x->next_snd_buf_ptr != NULL)
	{
		x->snd_sym = x->next_snd_sym;
		x->sync_val = 0.0;
		x->sync_step = x->next_sync_step;
		x->rec_position = 0;
		x->snd_buf_ptr = x->next_snd_buf_ptr;
		x->next_snd_buf_ptr = NULL;
		
		#ifdef DEBUG
			post("%s: new buffer~ > %s < is being used", OBJECT_NAME, x->snd_sym);
		#endif /* DEBUG */
		
		return true;
	}
	else
	{
		return false;
	}
}

/********************************************************************************
void recordplus_assist(t_recordplus *x, t_object *b, long msg, long arg, char *s)

inputs:			x		-- pointer to our object
				b		--
				msg		--
				arg		--
				s		--
description:	method called when "assist" message is received; allows inlets 
		and outlets to display assist messages as the mouse passes over them
returns:		nothing
********************************************************************************/
void recordplus_assist(t_recordplus *x, t_object *b, long msg, long arg, char *s)
{
	if (msg==ASSIST_INLET) {
		switch (arg) {
			case 0:
				strcpy(s, "(signal) control of recording");
				break;
			case 1:
				strcpy(s, "(signal) to be recorded");
				break;
			
		}
	} else if (msg==ASSIST_OUTLET) {
		switch (arg) {
			case 0:
				strcpy(s, "(signal) sync output");
				break;
			
		}
	}
	
	#ifdef DEBUG
		post("%s: assist message displayed", OBJECT_NAME);
	#endif /* DEBUG */
}

/********************************************************************************
void recordplus_getinfo(t_recordplus *x)

inputs:			x		-- pointer to our object
				
description:	method called when "getinfo" message is received; displays info
		about object and last update
returns:		nothing
********************************************************************************/
void recordplus_getinfo(t_recordplus *x)
{
	post("%s object by Nathan Wolek", OBJECT_NAME);
	post("Last updated on %s - www.nathanwolek.com", __DATE__);
}