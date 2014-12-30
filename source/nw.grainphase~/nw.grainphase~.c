/*
** grain.phase~.c
**
** MSP object
** sends out a continuous stream of grains 
** 
** 2001/03/29 started by Nathan Wolek
** 2002/07/11 buffer length no longer stored locally
** 2002/07/15 fixed mono buffer check problem
** 2002/07/25 added deferred buffer changes
** 2002/09/24 added getinfo message
** 2003/07/10 update to CW 8
** 2005/02/02 change to linear interp
** 2005/02/03 INTERP_ON is now default for windowing; added "b_inuse" check
** 2006/11/22 moved to Xcode; fixed buffer in use bug
** 
*/

#include "ext.h"		// required for all MAX external objects
#include "z_dsp.h"		// required for all MSP external objects
#include "buffer.h"		// required to deal with buffer object
#include <string.h>

//#define DEBUG			//enable debugging messages

#define OBJECT_NAME		"grain.phase~"		// name of the object

/* for the assist method */
#define ASSIST_INLET	1
#define ASSIST_OUTLET	2

/* for direction flag */
#define FORWARD_GRAINS		0
#define REVERSE_GRAINS		1

/* for interpolation flag */
#define INTERP_OFF			0
#define INTERP_ON			1

void *this_class;		// required global pointing to this class

typedef struct _grainphase
{
	t_pxobject x_obj;					// <--
	// sound buffer info
	t_symbol *snd_sym;
	t_buffer *snd_buf_ptr;
	t_buffer *next_snd_buf_ptr;
	//double snd_last_out;	//removed 2005.02.02
	//long snd_buf_length;	//removed 2002.07.11
	short snd_interp;
	// window buffer info
	t_symbol *win_sym;
	t_buffer *win_buf_ptr;
	t_buffer *next_win_buf_ptr;
	//double win_last_out;	//removed 2005.02.02
	double win_last_index;
	//long win_buf_length;	//removed 2002.07.11
	short win_interp;
	// grain info
	double grain_pos_start;	// in samples
	double grain_pitch;	// as multiplier, 0 to 1
	short grain_direction;	// forward or reverse
	double grain_sound_length; // in samples
	double curr_snd_pos; // in samples
	double snd_step_size; // in samples
	// defered grain info at control rate
	double next_grain_pos_start;	// in milliseconds
	double next_grain_pitch;		// as multiplier, 0 to 1
	short next_grain_direction;		// forward or reverse
	// signal or control grain info
	short grain_pos_start_connected;	// <--
	short grain_pitch_connected;		// <--
	double input_sr;					// <--
	double output_sr;					// <--
	double output_1oversr;				// <--
} t_grainphase;

void *grainphase_new(t_symbol *snd, t_symbol *win);
t_int *grainphase_perform(t_int *w);
t_int *grainphase_perform0(t_int *w);
void grainphase_dsp(t_grainphase *x, t_signal **sp, short *count);
void grainphase_setsnd(t_grainphase *x, t_symbol *s);
void grainphase_setwin(t_grainphase *x, t_symbol *s);
void grainphase_float(t_grainphase *x, double f);
void grainphase_int(t_grainphase *x, long l);
void grainphase_assist(t_grainphase *x, t_object *b, long msg, long arg, char *s);
void grainphase_getinfo(t_grainphase *x);
void grainphase_sndInterp(t_grainphase *x, long l);
void grainphase_winInterp(t_grainphase *x, long l);
void grainphase_reverse(t_grainphase *x, long l);
double mcLinearInterp(float *in_array, long index_i, double index_frac, long in_size, short in_chans);

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
	setup((t_messlist **)&this_class, (method)grainphase_new, (method)dsp_free, 
			(short)sizeof(t_grainphase), 0L, A_SYM, A_SYM, 0);
	addmess((method)grainphase_dsp, "dsp", A_CANT, 0);
	
	/* bind method "grainphase_setsnd" to the 'setSound' message */
	addmess((method)grainphase_setsnd, "setSound", A_SYM, 0);
	
	/* bind method "grainphase_setwin" to the 'setWin' message */
	addmess((method)grainphase_setwin, "setWin", A_SYM, 0);
	
	/* bind method "grainphase_float" to the incoming floats */
	addfloat((method)grainphase_float);
	
	/* bind method "grainphase_int" to the incoming ints */
	addfloat((method)grainphase_int);
	
	/* bind method "grainphase_assist" to the assistance message */
	addmess((method)grainphase_assist, "assist", A_CANT, 0);
	
	/* bind method "grainphase_getinfo" to the getinfo message */
	addmess((method)grainphase_getinfo, "getinfo", A_NOTHING, 0);
	
	/* bind method "grainphase_reverse" to the direction message */
	addmess((method)grainphase_reverse, "reverse", A_LONG, 0);
	
	/* bind method "grainphase_sndInterp" to the sndInterp message */
	addmess((method)grainphase_sndInterp, "sndInterp", A_LONG, 0);
	
	/* bind method "grainphase_winInterp" to the winInterp message */
	addmess((method)grainphase_winInterp, "winInterp", A_LONG, 0);
	
	dsp_initclass();
	
	/* needed for 'buffer~' work, checks for validity of buffer specified */
	ps_buffer = gensym("buffer~");
	
	#ifndef DEBUG
		
	#endif /* DEBUG */
}

/********************************************************************************
void *grainphase_new(double initial_pos)

inputs:			*snd		-- name of buffer holding sound
				*win		-- name of buffer holding window
description:	called for each new instance of object in the MAX environment;
		defines inlets and outlets; sets variables and buffers
returns:		nothing
********************************************************************************/
void *grainphase_new(t_symbol *snd, t_symbol *win)
{
	t_grainphase *x = (t_grainphase *)newobject(this_class);
	dsp_setup((t_pxobject *)x, 3);					// three inlets
	outlet_new((t_pxobject *)x, "signal");			// one outlet
	
	/* set buffer names */
	x->snd_sym = snd;
	x->win_sym = win;
	
	/* zero pointers */
	x->snd_buf_ptr = x->next_snd_buf_ptr = NULL;
	x->win_buf_ptr = x->next_win_buf_ptr = NULL;
	
	/* setup variables */
	x->grain_pos_start = x->next_grain_pos_start = 0.0;
	x->grain_pitch = x->next_grain_pitch = 1.0;
	x->curr_snd_pos = x->snd_step_size = 0.0; 
	
	/* set flags to defaults */
	x->snd_interp = INTERP_ON;
	x->win_interp = INTERP_ON;
	x->grain_direction = x->next_grain_direction = FORWARD_GRAINS;
	
	x->x_obj.z_misc = Z_NO_INPLACE;
	
	/* return a pointer to the new object */
	return (x);
}

/********************************************************************************
void grainphase_dsp(t_cpPan *x, t_signal **sp, short *count)

inputs:			x		-- pointer to this object
				sp		-- array of pointers to input & output signals
				count	-- array of shorts detailing number of signals attached
					to each inlet
description:	called when DSP call chain is built; adds object to signal flow
returns:		nothing
********************************************************************************/
void grainphase_dsp(t_grainphase *x, t_signal **sp, short *count)
{
	/* set buffers */
	grainphase_setsnd(x, x->snd_sym);
	grainphase_setwin(x, x->win_sym);
	
	/* test inlet 2 and 3 for signal data */
	x->grain_pos_start_connected = count[1];
	x->grain_pitch_connected = count[2];
	
	x->input_sr = sp[0]->s_sr;
	x->output_sr = sp[3]->s_sr;
	x->output_1oversr = 1.0 / x->output_sr;
	
	if (!count[3]) {	// nothing computed
		//dsp_add(grainphase_perform0, 2, sp[3]->s_vec, sp[3]->s_n+1);
		#ifdef DEBUG
			post("%s: no output computed", OBJECT_NAME);
		#endif /* DEBUG */
	} else {		// output computed
		dsp_add(grainphase_perform, 6, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec,
			sp[3]->s_vec, sp[0]->s_n);
		#ifdef DEBUG
			post("%s: output is being computed", OBJECT_NAME);
		#endif /* DEBUG */
	}
	
}

/********************************************************************************
t_int *grainphase_perform(t_int *w)

inputs:			w		-- array of signal vectors specified in "grainphase_dsp"
description:	called at interrupt level to compute object's output; used when
		outlets are connected; tests inlet 2 & 3 to use either control or audio
		rate data
returns:		pointer to the next 
********************************************************************************/
t_int *grainphase_perform(t_int *w)
{
	t_grainphase *x = (t_grainphase *)(w[1]);
	t_float *in_phasor = (t_float *)(w[2]);
	t_float *in_pos_start = (t_float *)(w[3]);
	t_float *in_pitch = (t_float *)(w[4]);
	t_float *out = (t_float *)(w[5]);
	int vec_size = (int)(w[6]) + 1;
	
	t_buffer *s_ptr = x->snd_buf_ptr;
	t_buffer *w_ptr = x->win_buf_ptr;
	float *tab_s, *tab_w;
	//double g_pos_start, g_pitch, g_sound_length; //removed 2002.07.25
	double  snd_out, win_out;// last_s, last_w;	//removed 2005.02.02
	double temp, index_s, index_w, last_index_w, s_step_size, temp_index_frac;
	long size_s, size_w, saveinuse_s, saveinuse_w, temp_index_int;
	short interp_s, interp_w, g_direction;
	
	if (x->x_obj.z_disabled)						// object is enabled
		goto out;
	if ((s_ptr == NULL) || (w_ptr == NULL))			// buffer names are defined
		goto zero;
	if (!s_ptr->b_valid || !w_ptr->b_valid)			// files are loaded
		goto zero;
		
	// set "in use" to true; added 2005.02.03
	saveinuse_s = s_ptr->b_inuse;
	s_ptr->b_inuse = true;
	saveinuse_w = w_ptr->b_inuse;
	w_ptr->b_inuse = true;
	
	tab_s = s_ptr->b_samples;
	tab_w = w_ptr->b_samples;
	size_s = s_ptr->b_frames;
	size_w = w_ptr->b_frames;
	//last_s = x->snd_last_out;	//removed 2005.02.02
	//last_w = x->win_last_out;	//removed 2005.02.02
	last_index_w = x->win_last_index;
	index_s = x->curr_snd_pos;
	s_step_size = x->snd_step_size;
	
	interp_s = x->snd_interp;
	interp_w = x->win_interp;
	g_direction = x->grain_direction;
	
	while (--vec_size) {
		temp = *in_phasor;
		temp *= size_w;
		index_w = temp;
		
		/* check bounds of window index */
		while (index_w < 0)
			index_w += size_w;
		while (index_w >= size_w)
			index_w -= size_w;
			
		if (index_w < last_index_w) {		// if window has wrapped...
			if (index_w < 10.0) {			// and is at beginning...
					
				if (x->next_snd_buf_ptr != NULL) {	//added 2002.07.24
					s_ptr->b_inuse = saveinuse_s;	// restore b_inuse; 2006.11.22
					x->snd_buf_ptr = x->next_snd_buf_ptr;
					x->next_snd_buf_ptr = NULL;
					//x->snd_last_out = 0.0;	//removed 2005.02.02
					s_ptr = x->snd_buf_ptr;
					saveinuse_s = s_ptr->b_inuse;	// 2006.11.22
					s_ptr->b_inuse = true;			// 2006.11.22
					
					#ifdef DEBUG
						post("%s: sound buffer pointer updated", OBJECT_NAME);
					#endif /* DEBUG */
				}
				if (x->next_win_buf_ptr != NULL) {	//added 2002.07.24
					w_ptr->b_inuse = saveinuse_w;	// restore b_inuse; 2006.11.22
					x->win_buf_ptr = x->next_win_buf_ptr;
					x->next_win_buf_ptr = NULL;
					//x->win_last_out = 0.0;	//removed 2005.02.02
					w_ptr = x->win_buf_ptr;
					saveinuse_w = w_ptr->b_inuse;	// 2006.11.22
					w_ptr->b_inuse = true;			// 2006.11.22
					
					#ifdef DEBUG
						post("%s: window buffer pointer updated", OBJECT_NAME);
					#endif /* DEBUG */
				}
				
				//
				//
				
				x->grain_direction = x->next_grain_direction;
				g_direction = x->grain_direction;
				
				// test if pitch should be at audio or control rate
				if (x->grain_pitch_connected) { // if pitch is at audio rate
					x->grain_pitch = *in_pitch;
				} else { // if pitch is at control rate
					x->grain_pitch = x->next_grain_pitch;
				}
				
				// compute sound buffer step size per output sample
				x->snd_step_size = x->grain_pitch * s_ptr->b_sr * x->output_1oversr;
				s_step_size = x->snd_step_size;
				
				// test if position should be at audio or control rate
				if (x->grain_pos_start_connected) { // if position is at audio rate
					if (g_direction == FORWARD_GRAINS) {	// if forward...
						x->grain_pos_start = *in_pos_start * s_ptr->b_msr;
						index_s = x->grain_pos_start - s_step_size;
					} else {	// if reverse...
						// estimate length of window for reversed grains
						temp = index_w - (last_index_w - size_w);
						temp *= size_w;
						x->grain_pos_start = (*in_pos_start * s_ptr->b_msr) + temp;
						index_s = x->grain_pos_start + s_step_size;
					}
				} else { // if position is at control rate
					if (g_direction == FORWARD_GRAINS) {	// if forward...
						x->grain_pos_start = x->next_grain_pos_start * s_ptr->b_msr;
						index_s = x->grain_pos_start - s_step_size;
					} else {	// if reverse...
						// estimate length of window for reversed grains
						temp = index_w - (last_index_w - size_w);
						temp *= size_w;
						x->grain_pos_start = (x->next_grain_pos_start * s_ptr->b_msr) + temp;
						index_s = x->grain_pos_start + s_step_size;
					}
				}
				
			}
		}
		
		/* handle temporary vars for interpolation */
		temp_index_int = (long)(index_w); // integer portion of index
		temp_index_frac = index_w - (double)temp_index_int; // fractional portion of index
		
		/*
		if (nc_w > 1) // if buffer has multiple channels...
		{
			// get index to sample from within the interleaved frame
			temp_index_int = temp_index_int * nc_w + chan_w;
		}
		*/
		
		switch (interp_w) {
			case INTERP_ON:
				// perform linear interpolation on window buffer output
				win_out = mcLinearInterp(tab_w, temp_index_int, temp_index_frac, size_w, 1);
				break;
			case INTERP_OFF:
				// interpolation sounds better than following, but uses more CPU
				win_out = tab_w[temp_index_int];
				break;
		}
		
		/* sound index is a double because it uses allPassInterp */
		if (g_direction == FORWARD_GRAINS) {	// if forward...
			index_s += s_step_size;		// add to sound index
		} else {	// if reverse...
			index_s -= s_step_size;		// subtract from sound index
		}
		
		/* check bounds of sound index */
		while (index_s < 0)
			index_s += size_s;
		while (index_s >= size_s)
			index_s -= size_s;
		
		/* handle temporary vars for interpolation */
		temp_index_int = (long)(index_s); // integer portion of index
		temp_index_frac = index_s - (double)temp_index_int; // fractional portion of index
		
		/*
		if (nc_s > 1) // if buffer has multiple channels...
		{
			// get index to sample from within the interleaved frame
			temp_index_int = temp_index_int * nc_s + chan_s;
		}
		*/
		
		switch (interp_s) {
			case INTERP_ON:
				// perform linear interpolation on sound buffer output
				snd_out = mcLinearInterp(tab_s, temp_index_int, temp_index_frac, size_s, 1);
				break;
			case INTERP_OFF:
				// interpolation sounds better than following, but uses more CPU
				snd_out = tab_s[temp_index_int];
				break;
		}
		
		/* multiply snd_out by win_out */
		*out = snd_out * win_out;
		
		/* update last output variables */
		//last_s = snd_out;	//removed 2005.02.02
		//last_w = win_out;	//removed 2005.02.02
		last_index_w = index_w;
		
		/* advance pointers*/
		++in_phasor, ++in_pos_start, ++in_pitch, ++out;
	}
	
	/* update last output variables */
	//x->snd_last_out = last_s;	//removed 2005.02.02
	//x->win_last_out = last_w;	//removed 2005.02.02
	x->win_last_index = index_w;
	x->curr_snd_pos = index_s;
	x->snd_step_size = s_step_size;
	
	// reset "in use"; added 2005.02.03
	s_ptr->b_inuse = saveinuse_s;
	w_ptr->b_inuse = saveinuse_w;
	
	return (w + 7);
	
zero:
	while (--vec_size >= 0) *++out = 0.;
out:
	return (w + 7);
}

/********************************************************************************
t_int *grainphase_perform0(t_int *w)

inputs:			w		-- array of signal vectors specified in "grainphase_dsp"
description:	called at interrupt level to compute object's output; used when
		nothing is connected to output; saves CPU cycles
returns:		pointer to the next 
********************************************************************************/
t_int *grainphase_perform0(t_int *w)
{
	t_float *out = (t_float *)(w[1]);
	int vec_size = (int)(w[2]) + 1;

	--out;

	while (--vec_size) {
		*++out = 0.;
	}

	return (w + 3);
}

/********************************************************************************
void grainphase_setsnd(t_index *x, t_symbol *s)

inputs:			x		-- pointer to this object
				s		-- name of buffer to link
description:	links buffer holding the grain sound source 
returns:		nothing
********************************************************************************/
void grainphase_setsnd(t_grainphase *x, t_symbol *s)
{
	t_buffer *b;
	
	if ((b = (t_buffer *)(s->s_thing)) && ob_sym(b) == ps_buffer) {
		if (b->b_nchans != 1) {
			error("%s: buffer~ > %s < must be mono", OBJECT_NAME, s->s_name);
			x->next_snd_buf_ptr = NULL;		//added 2002.07.15
		} else {
			if (x->snd_buf_ptr == NULL) { // if first buffer make current buffer
				x->snd_sym = s;
				x->snd_buf_ptr = b;
				//x->snd_last_out = 0.0;	//removed 2005.02.02
				
				#ifdef DEBUG
					post("%s: current sound set to buffer~ > %s <", OBJECT_NAME, s->s_name);
				#endif /* DEBUG */
			} else { // else defer to next buffer
				x->snd_sym = s;
				x->next_snd_buf_ptr = b;
				//x->snd_buf_length = b->b_frames;	//removed 2002.07.11
				//x->snd_last_out = 0.0;		//removed 2002.07.24
				
				#ifdef DEBUG
					post("%s: next sound set to buffer~ > %s <", OBJECT_NAME, s->s_name);
				#endif /* DEBUG */
			}
		}
	} else {
		error("%s: no buffer~ * %s * found", OBJECT_NAME, s->s_name);
		x->next_snd_buf_ptr = NULL;
	}
}

/********************************************************************************
void grainphase_setwin(t_grainphase *x, t_symbol *s)

inputs:			x		-- pointer to this object
				s		-- name of buffer to link
description:	links buffer holding the grain window 
returns:		nothing
********************************************************************************/
void grainphase_setwin(t_grainphase *x, t_symbol *s)
{
	t_buffer *b;
	
	if ((b = (t_buffer *)(s->s_thing)) && ob_sym(b) == ps_buffer) {
		if (b->b_nchans != 1) {
			error("%s: buffer~ > %s < must be mono", OBJECT_NAME, s->s_name);
			x->next_win_buf_ptr = NULL;		//added 2002.07.15
		} else {
			if (x->win_buf_ptr == NULL) { // if first buffer make current buffer
				x->win_sym = s;
				x->win_buf_ptr = b;
				//x->win_last_out = 0.0;	//removed 2005.02.02
				x->win_last_index = b->b_frames;
				
				#ifdef DEBUG
					post("%s: current window set to buffer~ > %s <", OBJECT_NAME, s->s_name);
				#endif /* DEBUG */
			} else { // else defer to next buffer
				x->win_sym = s;
				x->next_win_buf_ptr = b;
				//x->win_buf_length = b->b_frames;	//removed 2002.07.11
				//x->win_last_out = 0.0;		//removed 2002.07.24
				
				#ifdef DEBUG
					post("%s: next window set to buffer~ > %s <", OBJECT_NAME, s->s_name);
				#endif /* DEBUG */
			}
		}
	} else {
		error("%s: no buffer~ > %s < found", OBJECT_NAME, s->s_name);
		x->next_win_buf_ptr = NULL;
	}
}

/********************************************************************************
void grainphase_float(t_grainphase *x, double f)

inputs:			x		-- pointer to our object
				f		-- value of float input
description:	handles floats sent to inlets; inlet 2 sets "next_grain_pos_start" 
		variable; inlet 3 sets "next_grain_length" variable; left inlet generates 
		error message in max window
returns:		nothing
********************************************************************************/
void grainphase_float(t_grainphase *x, double f)
{
	if (x->x_obj.z_in == 1) // if inlet 2
	{
		x->next_grain_pos_start = f;
	}
	else if (x->x_obj.z_in == 2) // if inlet 3
	{
		x->next_grain_pitch = f;
	}
	else if (x->x_obj.z_in == 0)
	{
		post("%s: left inlet does not accept floats", OBJECT_NAME);
	}
}

/********************************************************************************
void grainphase_int(t_grainphase *x, long l)

inputs:			x		-- pointer to our object
				l		-- value of int input
description:	handles int sent to inlets; inlet 2 sets "next_grain_pos_start" 
		variable; inlet 3 sets "next_grain_length" variable; left inlet generates 
		error message in max window
returns:		nothing
********************************************************************************/
void grainphase_int(t_grainphase *x, long l)
{
	if (x->x_obj.z_in == 1) // if inlet 2
	{
		x->next_grain_pos_start = (double) l;
	}
	else if (x->x_obj.z_in == 2) // if inlet 3
	{
		x->next_grain_pitch = (double) l;
	}
	else if (x->x_obj.z_in == 0)
	{
		post("%s: left inlet does not accept floats", OBJECT_NAME);
	}
}

/********************************************************************************
void grainphase_assist(t_grainphase *x, t_object *b, long msg, long arg, char *s)

inputs:			x		-- pointer to our object
				b		--
				msg		--
				arg		--
				s		--
description:	method called when "assist" message is received; allows inlets 
		and outlets to display assist messages as the mouse passes over them
returns:		nothing
********************************************************************************/
void grainphase_assist(t_grainphase *x, t_object *b, long msg, long arg, char *s)
{
	if (msg==ASSIST_INLET) {
		switch (arg) {
			case 0:
				strcpy(s, "(signal) phase, 0 to 1");
				break;
			case 1:
				strcpy(s, "(signal/float) sound begin, in milliseconds");
				break;
			case 2:
				strcpy(s, "(signal/float) grain pitch multiplier, 1.0 = unchanged");
				break;
		}
	} else if (msg==ASSIST_OUTLET) {
		switch (arg) {
			case 0:
				strcpy(s, "(signal) grain output");
				break;
		}
	}
	
	#ifdef DEBUG
		post("%s: assist message displayed", OBJECT_NAME);
	#endif /* DEBUG */
}

/********************************************************************************
void grainphase_getinfo(t_grainphase *x)

inputs:			x		-- pointer to our object
				
description:	method called when "getinfo" message is received; displays info
		about object and last update
returns:		nothing
********************************************************************************/
void grainphase_getinfo(t_grainphase *x)
{
	post("%s object by Nathan Wolek", OBJECT_NAME);
	post("Last updated on %s - www.nathanwolek.com", __DATE__);
}

/********************************************************************************
void grainphase_sndInterp(t_grainphase *x, long l)

inputs:			x		-- pointer to our object
				l		-- flag value
description:	method called when "sndInterp" message is received; allows user 
		to define whether interpolation is used in pulling values from the sound
		buffer; default is on
returns:		nothing
********************************************************************************/
void grainphase_sndInterp(t_grainphase *x, long l)
{
	if (l == INTERP_OFF) {
		x->snd_interp = INTERP_OFF;
		#ifdef DEBUG
			post("%s: sndInterp is set to off", OBJECT_NAME);
		#endif // DEBUG //
	} else if (l == INTERP_ON) {
		x->snd_interp = INTERP_ON;
		#ifdef DEBUG
			post("%s: sndInterp is set to on", OBJECT_NAME);
		#endif // DEBUG //
	} else {
		error("%s: sndInterp message was not understood", OBJECT_NAME);
	}
}

/********************************************************************************
void grainphase_winInterp(t_grainphase *x, long l)

inputs:			x		-- pointer to our object
				l		-- flag value
description:	method called when "winInterp" message is received; allows user 
		to define whether interpolation is used in pulling values from the window
		buffer; default is on
returns:		nothing
********************************************************************************/
void grainphase_winInterp(t_grainphase *x, long l)
{
	if (l == INTERP_OFF) {
		x->win_interp = INTERP_OFF;
		#ifdef DEBUG
			post("%s: winInterp is set to off", OBJECT_NAME);
		#endif // DEBUG //
	} else if (l == INTERP_ON) {
		x->win_interp = INTERP_ON;
		#ifdef DEBUG
			post("%s: winInterp is set to on", OBJECT_NAME);
		#endif // DEBUG //
	} else {
		error("%s: winInterp was not understood", OBJECT_NAME);
	}
}

/********************************************************************************
void grainphase_reverse(t_grainphase *x, long l)

inputs:			x		-- pointer to our object
				l		-- flag value
description:	method called when "reverse" message is received; allows user 
		to define whether sound is played forward or reverse; default is forward
returns:		nothing
********************************************************************************/
void grainphase_reverse(t_grainphase *x, long l)
{
	if (l == REVERSE_GRAINS) {
		x->next_grain_direction = REVERSE_GRAINS;
		#ifdef DEBUG
			post("%s: reverse is set to on", OBJECT_NAME);
		#endif // DEBUG //
	} else if (l == FORWARD_GRAINS) {
		x->next_grain_direction = FORWARD_GRAINS;
		#ifdef DEBUG
			post("%s: reverse is set to off", OBJECT_NAME);
		#endif // DEBUG //
	} else {
		error("%s: reverse was not understood", OBJECT_NAME);
	}
	
}

/********************************************************************************
double mcLinearInterp(float *in_array, long index_i, double index_frac, 
		long in_size, short in_chans)

inputs:			*in_array -- name of array of input values
				index_i -- index value of sample, specific channel within interleaved frame
				index_frac -- fractional portion of index value for interp
				in_size -- size of input buffer to perform wrapping
				in_chans -- number of channels in input buffer
description:	performs linear interpolation on an input array and to return 
	value of a fractional sample location
returns:		interpolated output
********************************************************************************/
double mcLinearInterp(float *in_array, long index_i, double index_frac, long in_size, short in_chans)
{
	double out, sample1, sample2;
	long index_iP1 = index_i + in_chans;		// corresponding sample in next frame
	
	// make sure that index_iP1 is not out of range
	while (index_iP1 >= in_size) index_iP1 -= in_size;
	
	// get samples
	sample1 = (double)in_array[index_i];
	sample2 = (double)in_array[index_iP1];
	
	//linear interp formula
	out = sample1 + index_frac * (sample2 - sample1);
	
	return out;
}