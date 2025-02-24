/* Copyright (C) 2001-2023 Artifex Software, Inc.
   All Rights Reserved.

   This software is provided AS-IS with no warranty, either express or
   implied.

   This software is distributed under license and may not be copied,
   modified or distributed except as expressly authorized under the terms
   of the license contained in the file LICENSE in this distribution.

   Refer to licensing information at http://www.artifex.com or contact
   Artifex Software, Inc.,  39 Mesa Street, Suite 108A, San Francisco,
   CA 94129, USA, for further information.
*/


/* Level 2 sethalftone operator */
#include "memory_.h"
#include "ghost.h"
#include "oper.h"
#include "gsstruct.h"
#include "gxdevice.h"		/* for gzht.h */
#include "gzht.h"
#include "estack.h"
#include "ialloc.h"
#include "iddict.h"
#include "idparam.h"
#include "igstate.h"
#include "icolor.h"
#include "iht.h"
#include "store.h"
#include "iname.h"
#include "zht2.h"
#include "gxgstate.h"
#include "gen_ordered.h"
#include "gp.h"

/* Forward references */
static int dict_spot_params(const ref *, gs_spot_halftone *, ref *, ref *,
                            gs_memory_t *);
static int dict_spot_results(i_ctx_t *, ref *, const gs_spot_halftone *);
static int dict_threshold_params(const ref *, gs_threshold_halftone *,
                                  ref *);
static int dict_threshold2_params(const ref *, gs_threshold2_halftone *,
                                   ref *, gs_memory_t *);

/*
 * This routine translates a gs_separation_name value into a character string
 * pointer and a string length.
 */
int
gs_get_colorname_string(gs_gstate *pgs, gs_separation_name colorname_index,
                        unsigned char **ppstr, unsigned int *pname_size)
{
    ref nref;

    name_index_ref(pgs->memory, colorname_index, &nref);
    name_string_ref(pgs->memory, &nref, &nref);
    return obj_string_data(pgs->memory, &nref, (const unsigned char**) ppstr, pname_size);
}

/* Dummy spot function */
static float
spot1_dummy(double x, double y)
{
    return (x + y) / 2;
}

static int
ht_object_type_from_name(gs_ref_memory_t *mem, ref *pname, gs_HT_objtype_t *HTobjtype)
{
    ref sref;

    *HTobjtype = HT_OBJTYPE_DEFAULT;
    name_string_ref(mem, pname, &sref);
    if (r_size(&sref) <= 1)
        return_error(gs_error_undefined);	/* PDF allows zero length strings, but it can't match */

    switch (sref.value.const_bytes[0]) {
	case 'D':
            if (r_size(&sref) == 7 && strncmp((const char *)sref.value.const_bytes, "Default", 7) == 0) {
                *HTobjtype = HT_OBJTYPE_DEFAULT;
		break;
            }
            return_error(gs_error_undefined);
	case 'V':
            if (r_size(&sref) == 6 && strncmp((const char *)sref.value.const_bytes, "Vector", 6) == 0) {
                *HTobjtype = HT_OBJTYPE_VECTOR;
		break;
            }
            return_error(gs_error_undefined);
	case 'I':
            if (r_size(&sref) == 5 && strncmp((const char *)sref.value.const_bytes, "Image", 5) == 0) {
                *HTobjtype = HT_OBJTYPE_IMAGE;
		break;
            }
            return_error(gs_error_undefined);
	case 'T':
            if (r_size(&sref) == 4 && strncmp((const char *)sref.value.const_bytes, "Text", 4) == 0) {
                *HTobjtype = HT_OBJTYPE_TEXT;
		break;
            }	/* falls through to default if no match */
        default:
            return_error(gs_error_undefined);
    }
    return 0;
}

/* <dict> <dict5> .sethalftone5 - */
static int sethalftone_finish(i_ctx_t *);
static int sethalftone_cleanup(i_ctx_t *);
static int
zsethalftone5(i_ctx_t *i_ctx_p)
{
    os_ptr op = osp;
    uint count;
    gs_halftone_component *phtc = 0;
    gs_halftone_component *pc;
    int code = 0;
    int j;
    bool have_default;
    gs_halftone *pht = 0;
    gx_device_halftone *pdht = 0;
    ref sprocs[GS_CLIENT_COLOR_MAX_COMPONENTS + 1];
    ref tprocs[GS_CLIENT_COLOR_MAX_COMPONENTS + 1];
    gs_memory_t *mem;
    uint edepth = ref_stack_count(&e_stack);
    int npop = 2;
    int dict_enum;
    ref rvalue[2];
    int cname, colorant_number;
    byte * pname;
    uint name_size;
    int halftonetype, type = 0;
    gs_HT_objtype_t objtype = HT_OBJTYPE_DEFAULT;
    ref *pdval;
    gs_gstate *pgs = igs;
    int space_index;

    if (ref_stack_count(&o_stack) < 2)
        return_error(gs_error_stackunderflow);
    check_type(*op, t_dictionary);
    check_type(*(op - 1), t_dictionary);

    dict_enum = dict_first(op);
    space_index = r_space_index(op - 1);

    mem = (gs_memory_t *) idmemory->spaces_indexed[space_index];

    check_type(*op, t_dictionary);
    check_dict_read(*op);
    check_type(op[-1], t_dictionary);
    check_dict_read(op[-1]);

    /*
     * We think that Type 2 and Type 4 halftones, like
     * screens set by setcolorscreen, adapt automatically to
     * the device color space, so we need to mark them
     * with a different internal halftone type.
     */
    code = dict_int_param(op - 1, "HalftoneType", 1, 100, 0, &type);
    if (code < 0)
          return code;
    halftonetype = (type == 2 || type == 4)
                        ? ht_type_multiple_colorscreen
                        : ht_type_multiple;

    /* Check if this dict has the optional ObjectType parameter */
    if (dict_find_string(op - 1, "ObjectType", &pdval) > 0 &&
        r_has_type(pdval, t_name)) {
        if ((code = ht_object_type_from_name(iimemory, pdval, &objtype)) < 0)
            return code;
    }

    /* Count how many components that we will actually use. */

    have_default = false;
    for (count = 0; ;) {

        /* Move to next element in the dictionary */
        if ((dict_enum = dict_next(op, dict_enum, rvalue)) == -1)
            break;
        /*
         * Verify that we have a valid component.  We may have a
         * /HalfToneType entry.
         */
        if (!r_has_type(&rvalue[0], t_name))
            continue;
        if (!r_has_type(&rvalue[1], t_dictionary))
            continue;

        /* Get the name of the component  verify that we will use it. */
        cname = name_index(mem, &rvalue[0]);
        code = gs_get_colorname_string(pgs, cname, &pname, &name_size);
        if (code < 0)
            break;
        colorant_number = gs_cname_to_colorant_number(pgs, pname, name_size,
                                                halftonetype);
        if (colorant_number < 0)
            continue;
        else if (colorant_number == GX_DEVICE_COLOR_MAX_COMPONENTS) {
            /* If here then we have the "Default" component */
            if (have_default)
                return_error(gs_error_rangecheck);
            have_default = true;
        }

        count++;
        /*
         * Check to see if we have already reached the legal number of
         * components.
         */
        if (count > GS_CLIENT_COLOR_MAX_COMPONENTS + 1) {
            code = gs_note_error(gs_error_rangecheck);
            break;
        }
    }
    if (count == 0 || (halftonetype == ht_type_multiple && ! have_default))
        code = gs_note_error(gs_error_rangecheck);

    if (code >= 0) {
        check_estack(5);		/* for sampling Type 1 screens */
        refset_null(sprocs, count);
        refset_null(tprocs, count);
        rc_alloc_struct_0(pht, gs_halftone, &st_halftone,
                          imemory, pht = 0, ".sethalftone5");
        phtc = gs_alloc_struct_array(mem, count, gs_halftone_component,
                                     &st_ht_component_element,
                                     ".sethalftone5");
        rc_alloc_struct_0(pdht, gx_device_halftone, &st_device_halftone,
                          imemory, pdht = 0, ".sethalftone5");
        if (pht == 0 || phtc == 0 || pdht == 0) {
            j = 0; /* Quiet the compiler:
                      gs_note_error isn't necessarily identity,
                      so j could be left ununitialized. */
            code = gs_note_error(gs_error_VMerror);
        }
    }
    if (code >= 0) {
        dict_enum = dict_first(op);
        for (j = 0, pc = phtc; ;) {
            int type;

            /* Move to next element in the dictionary */
            if ((dict_enum = dict_next(op, dict_enum, rvalue)) == -1)
                break;
            /*
             * Verify that we have a valid component.  We may have a
             * /HalfToneType entry.
             */
            if (!r_has_type(&rvalue[0], t_name))
                continue;
            if (!r_has_type(&rvalue[1], t_dictionary))
                continue;

            /* Get the name of the component */
            cname = name_index(mem, &rvalue[0]);
            code = gs_get_colorname_string(pgs, cname, &pname, &name_size);
            if (code < 0)
                break;
            colorant_number = gs_cname_to_colorant_number(pgs, pname, name_size,
                                                halftonetype);
            if (colorant_number < 0)
                continue;		/* Do not use this component */
            pc->cname = cname;
            pc->comp_number = colorant_number;

            /* Now process the component dictionary */
            check_dict_read(rvalue[1]);
            if (dict_int_param(&rvalue[1], "HalftoneType", 1, 7, 0, &type) < 0) {
                code = gs_note_error(gs_error_typecheck);
                break;
            }
            switch (type) {
                default:
                    code = gs_note_error(gs_error_rangecheck);
                    break;
                case 1:
                    code = dict_spot_params(&rvalue[1], &pc->params.spot,
                                                sprocs + j, tprocs + j, mem);
                    pc->params.spot.screen.spot_function = spot1_dummy;
                    pc->type = ht_type_spot;
                    break;
                case 3:
                    code = dict_threshold_params(&rvalue[1], &pc->params.threshold,
                                                        tprocs + j);
                    pc->type = ht_type_threshold;
                    break;
                case 7:
                    code = dict_threshold2_params(&rvalue[1], &pc->params.threshold2,
                                                        tprocs + j, imemory);
                    pc->type = ht_type_threshold2;
                    break;
            }
            if (code < 0)
                break;
            pc++;
            j++;
        }
    }
    if (code >= 0) {
        pht->type = halftonetype;
        pht->objtype = objtype;
        pht->params.multiple.components = phtc;
        pht->params.multiple.num_comp = j;
        pht->params.multiple.get_colorname_string = gs_get_colorname_string;
        code = gs_sethalftone_prepare(igs, pht, pdht);
    }
    if (code >= 0) {
        /*
         * Put the actual frequency and angle in the spot function component dictionaries.
         */
        dict_enum = dict_first(op);
        for (pc = phtc; ; ) {
            /* Move to next element in the dictionary */
            if ((dict_enum = dict_next(op, dict_enum, rvalue)) == -1)
                break;

            /* Verify that we have a valid component */
            if (!r_has_type(&rvalue[0], t_name))
                continue;
            if (!r_has_type(&rvalue[1], t_dictionary))
                continue;

            /* Get the name of the component and verify that we will use it. */
            cname = name_index(mem, &rvalue[0]);
            code = gs_get_colorname_string(pgs, cname, &pname, &name_size);
            if (code < 0)
                break;
            colorant_number = gs_cname_to_colorant_number(pgs, pname, name_size,
                                                halftonetype);
            if (colorant_number < 0)
                continue;

            if (pc->type == ht_type_spot) {
                code = dict_spot_results(i_ctx_p, &rvalue[1], &pc->params.spot);
                if (code < 0)
                    break;
            }
            pc++;
        }
    }
    if (code >= 0) {
        /*
         * Schedule the sampling of any Type 1 screens,
         * and any (Type 1 or Type 3) TransferFunctions.
         * Save the stack depths in case we have to back out.
         */
        uint odepth = ref_stack_count(&o_stack);
        ref odict, odict5;

        odict = op[-1];
        odict5 = *op;
        ref_stack_pop(&o_stack, 2);
        op = osp;
        esp += 5;
        make_mark_estack(esp - 4, es_other, sethalftone_cleanup);
        esp[-3] = odict;
        make_istruct(esp - 2, 0, pht);
        make_istruct(esp - 1, 0, pdht);
        make_op_estack(esp, sethalftone_finish);
        for (j = 0; j < count; j++) {
            gx_ht_order *porder = NULL;

            if (pdht->components == 0)
                porder = &pdht->order;
            else {
                /* Find the component in pdht that matches component j in
                   the pht; gs_sethalftone_prepare() may permute these. */
                int k;
                int comp_number = phtc[j].comp_number;
                for (k = 0; k < count; k++) {
                    if (pdht->components[k].comp_number == comp_number) {
                        porder = &pdht->components[k].corder;
                        break;
                    }
                }
            }
            switch (phtc[j].type) {
            case ht_type_spot:
                code = zscreen_enum_init(i_ctx_p, porder,
                                         &phtc[j].params.spot.screen,
                                         &sprocs[j], 0, 0, space_index);
                if (code < 0)
                    break;
                /* falls through */
            case ht_type_threshold:
            case ht_type_threshold2:
                if (!r_has_type(tprocs + j, t__invalid)) {
                    /* Schedule TransferFunction sampling. */
                    /****** check_xstack IS WRONG ******/
                    check_ostack(zcolor_remap_one_ostack);
                    check_estack(zcolor_remap_one_estack);
                    code = zcolor_remap_one(i_ctx_p, tprocs + j,
                                            porder->transfer, igs,
                                            zcolor_remap_one_finish);
                    op = osp;
                }
                break;
            default:	/* not possible here, but to keep */
                                /* the compilers happy.... */
                ;
            }
            if (code < 0) {	/* Restore the stack. */
                ref_stack_pop_to(&o_stack, odepth);
                ref_stack_pop_to(&e_stack, edepth);
                op = osp;
                op[-1] = odict;
                *op = odict5;
                break;
            }
            npop = 0;
        }
    }
    if (code < 0) {
        gs_free_object(mem, pdht, ".sethalftone5");
        gs_free_object(mem, phtc, ".sethalftone5");
        gs_free_object(mem, pht, ".sethalftone5");
        return code;
    }
    pop(npop);
    return (ref_stack_count(&e_stack) > edepth ? o_push_estack : 0);
}

/* <dict> .genordered <string> */
/*         array will have: width height turn_on_sequence.x turn_on_sequence.y ...	*/
/*         total array length is 2 + (2 * width * height)				*/
static int
zgenordered(i_ctx_t *i_ctx_p)
{
    os_ptr op = osp;
    int i, code = 0;
    gs_memory_t *mem;
    int space_index;
    htsc_param_t params;
    int S;
    htsc_dig_grid_t final_mask;
    float tmp_float;
    gs_gstate *pgs = igs;
    gx_device *currdevice = pgs->device;
    output_format_type output_type = OUTPUT_PS;
    ref *out_type_name;

    if (ref_stack_count(&o_stack) < 1)
        return_error(gs_error_stackunderflow);
    check_type(*op, t_dictionary);

    space_index = r_space_index(op);		/* used to construct array that is returned */
    mem = (gs_memory_t *) idmemory->spaces_indexed[space_index];

    check_type(*op, t_dictionary);
    check_dict_read(*op);

    htsc_set_default_params(&params);
    /* Modify the default HResolution and VResolution to be the device HWResolution */
    params.horiz_dpi = currdevice->HWResolution[0];
    params.vert_dpi = currdevice->HWResolution[1];
    final_mask.memory = mem->non_gc_memory;
    final_mask.data = NULL;

    if ((code = dict_find_string(op, "OutputType", &out_type_name)) > 0) {
        ref namestr;

        if (!r_has_type(out_type_name, t_name))
            return gs_error_typecheck;
        name_string_ref(imemory, out_type_name, &namestr);
        if (r_size(&namestr) == 8 && !memcmp(namestr.value.bytes, "TOSArray", 8))
            output_type = OUTPUT_TOS;
        else if (r_size(&namestr) == 5 && !memcmp(namestr.value.bytes, "Type3", 5))
            output_type = OUTPUT_PS;
        else if (r_size(&namestr) == 12 && !memcmp(namestr.value.bytes, "ThreshString", 12))
            output_type = OUTPUT_RAW;
        else
            return gs_error_undefined;
    }
    if ((code = dict_int_param(op, "Angle", 0, 360, 0, &params.targ_scr_ang)) < 0)
        return gs_error_undefined;
    if ((code = dict_int_param(op, "Frequency", 1, 0x7fff, 75, &params.targ_lpi)) < 0)
        return gs_error_undefined;
    if ((code = dict_float_param(op, "HResolution", 300., &tmp_float)) < 0)
        return gs_error_undefined;
    if (code == 0)
        params.horiz_dpi = tmp_float;
    if ((code = dict_float_param(op, "VResolution", 300., &tmp_float)) < 0)
        return gs_error_undefined;
    if (code == 0)
        params.vert_dpi = tmp_float;
    if ((code = dict_int_param(op, "Levels", 1, 0x7fff, 256, &params.targ_quant)) < 0)
        return gs_error_undefined;
    if (code == 0)
        params.targ_quant_spec = true;
    if ((code = dict_int_param(op, "SuperCellSize", 1, 0x7fff, 1, &params.targ_size)) < 0)
        return gs_error_undefined;
    if (code == 0)
        params.targ_size_spec = true;
    if ((code = dict_int_param(op, "DotShape", 0, CUSTOM - 1, 0, (int *)(&params.spot_type))) < 0)
        return gs_error_undefined;
    if ((code = dict_bool_param(op, "Holladay", false, &params.holladay)) < 0)
        return gs_error_undefined;

    params.output_format = OUTPUT_TOS;		/* we want this format */
    code = htsc_gen_ordered(params, &S, &final_mask, mem);

#if FINAL_SCREEN_DUMP
    if (code >= 0) {
        code = htsc_save_screen(&final_mask, params.holladay, S, params, mem);
    }
#endif

    if (code < 0)
        goto done;

    switch (output_type) {
      case OUTPUT_TOS:
        /* Now return the mask info in an array [ width height turn_on.x turn_on.y ... ] */
        code = ialloc_ref_array((ref *)op, a_all, 2 + (2 * final_mask.width * final_mask.height), "gen_ordered");
        if (code < 0)
            goto done;
        make_int(&(op->value.refs[0]), final_mask.width);
        make_int(&(op->value.refs[1]), final_mask.height);
        for (i=0; i < 2 * final_mask.width * final_mask.height; i++)
            make_int(&(op->value.refs[i+2]), final_mask.data[i]);
        break;
      case OUTPUT_RAW:
      case OUTPUT_PS:
    /* Return a threshold array string first two bytes are width (high byte first),
     * next two bytes are height, followed by the threshold array (one byte per cell)
     * PostScript can easily form a Type 3 Halftone Thresholds string from this
     * using "getinterval".
     */
        {
            /* Make a threshold array from the turn_on_sequence */
            int level;
            int cur_pix = 0;
            int width = final_mask.width;
            int num_pix = width * final_mask.height;
            double delta_value = 1.0 / (double)(num_pix);
            double end_value, cur_value = 0.0;
            byte *thresh;
            ref rval, thresh_ref;

            code = gs_error_VMerror;	/* in case allocation of thresh fails */
            if (output_type == OUTPUT_RAW) {
                if ((thresh = ialloc_string(4 + num_pix, "gen_ordered"))  == 0)
                    goto done;
                *thresh++ = width >> 8;
                *thresh++ = width & 0xff;
                *thresh++ = final_mask.height >> 8;
                *thresh++ = final_mask.height & 0xff;
            } else if ((thresh = ialloc_string(num_pix, "gen_ordered"))  == 0)
                    goto done;
            /* The following is adapted from thresh_remap with the default linear map */
            for (level=0; level<256; level++) {
                end_value = (float)(1+level) / 255.;
                if (end_value > 255.0)
                    end_value = 255.0;		/* clamp in case of rounding errors */
                while (cur_value < (end_value - (delta_value * (1./256.))) ||
                       (cur_pix + 1) == (num_pix / 2) ) {	/* force 50% gray level */
                    thresh[final_mask.data[2*cur_pix] + (width*final_mask.data[2*cur_pix+1])] = 255 - level;
                    cur_pix++;
                    if (cur_pix >= num_pix)
                        break;
                    cur_value += delta_value;
                }
                if (cur_pix >= num_pix)
                    break;
            }
            /* now fill any remaining cells */
            for (; cur_pix < num_pix; cur_pix++) {
                thresh[final_mask.data[2 * cur_pix] + (width*final_mask.data[2 * cur_pix + 1])] = 0;
            }
#if FINAL_SCREEN_DUMP
            {
                char file_name[FULL_FILE_NAME_LENGTH];
                gp_file *fid;

                snprintf(file_name, FULL_FILE_NAME_LENGTH, "Screen_%dx%d.raw", width, final_mask.height);
                fid = gp_fopen(mem, file_name, "wb");
                if (fid) {
                    gp_fwrite(thresh, sizeof(unsigned char), num_pix, fid);
                    gp_fclose(fid);
                }
            }
#endif
            if (output_type == OUTPUT_RAW) {
                make_string(&thresh_ref, a_all | icurrent_space, 4 + num_pix, thresh-4);
                *op = thresh_ref;
                code = 0;
            } else {
                /* output_type == OUTPUT_PS */
                /* Return a HalftoneType 3 dictionary */
                code = dict_create(4, op);
                if (code < 0)
                    goto done;
                make_string(&thresh_ref, a_all | icurrent_space, num_pix, thresh);
                if ((code = idict_put_string(op, "Thresholds", &thresh_ref)) < 0)
                    goto done;
                make_int(&rval, final_mask.width);
                if ((code = idict_put_string(op, "Width", &rval)) < 0)
                    goto done;
                make_int(&rval, final_mask.height);
                if ((code = idict_put_string(op, "Height", &rval)) < 0)
                    goto done;
                make_int(&rval, 3);
                if ((code = idict_put_string(op, "HalftoneType", &rval)) < 0)
                    goto done;
            }
        }
        break;
      default:
        return gs_error_undefined;
    }

done:
    if (final_mask.data != NULL)
        gs_free_object(mem->non_gc_memory, final_mask.data, ".genordered");

    return (code < 0 ? gs_error_undefined : 0);
}

/* Install the halftone after sampling. */
static int
sethalftone_finish(i_ctx_t *i_ctx_p)
{
    gx_device_halftone *pdht = r_ptr(esp, gx_device_halftone);
    int code;

    if (pdht->components)
        pdht->order = pdht->components[0].corder;
    code = gx_ht_install(igs, r_ptr(esp - 1, gs_halftone), pdht);
    if (code < 0) {
        esp -= 4;
        sethalftone_cleanup(i_ctx_p);
        return code;
    }
    istate->halftone = esp[-2];
    esp -= 4;
    sethalftone_cleanup(i_ctx_p);
    return o_pop_estack;
}
/* Clean up after installing the halftone. */
static int
sethalftone_cleanup(i_ctx_t *i_ctx_p)
{
    gx_device_halftone *pdht = r_ptr(&esp[4], gx_device_halftone);
    gs_halftone *pht = r_ptr(&esp[3], gs_halftone);

    gs_free_object(pdht->rc.memory, pdht,
                   "sethalftone_cleanup(device halftone)");
    gs_free_object(pht->rc.memory, pht,
                   "sethalftone_cleanup(halftone)");
    return 0;
}

static int
zsetobjtypeHT(i_ctx_t *i_ctx_p)		/* <name> .setobjtypeHT - */
                                        /* name is one of /Vector, /Image, or /Text */
{
    os_ptr op = osp;
    int code = 0;
    gs_HT_objtype_t HTobjtype = HT_OBJTYPE_DEFAULT;

    if (ref_stack_count(&o_stack) < 1)
        return_error(gs_error_stackunderflow);
    check_type(*op, t_name);

    if ((code = ht_object_type_from_name(iimemory, op, &HTobjtype)) < 0)
        return code;

    /* If we made it this far, HTobjtype is valid */
    code = gx_gstate_dev_ht_copy_to_objtype(i_ctx_p->pgs, HTobjtype);
    if (code < 0)
        return code;

    pop(1);
    return 0;
}

/* ------ Initialization procedure ------ */

const op_def zht2_l2_op_defs[] =
{
    op_def_begin_level2(),
    {"2.sethalftone5", zsethalftone5},
    {"1.genordered", zgenordered},
    {"1.setobjtypeHT", zsetobjtypeHT},
                /* Internal operators */
    {"0%sethalftone_finish", sethalftone_finish},
    op_def_end(0)
};

/* ------ Internal routines ------ */

/* Extract frequency, angle, spot function, and accurate screens flag */
/* from a dictionary. */
static int
dict_spot_params(const ref * pdict, gs_spot_halftone * psp,
                 ref * psproc, ref * ptproc, gs_memory_t *mem)
{
    int code;

    check_dict_read(*pdict);
    if ((code = dict_float_param(pdict, "Frequency", 0.0,
                                 &psp->screen.frequency)) != 0 ||
        (code = dict_float_param(pdict, "Angle", 0.0,
                                 &psp->screen.angle)) != 0 ||
      (code = dict_proc_param(pdict, "SpotFunction", psproc, false)) != 0 ||
        (code = dict_bool_param(pdict, "AccurateScreens",
                                gs_currentaccuratescreens(mem),
                                &psp->accurate_screens)) < 0 ||
      (code = dict_proc_param(pdict, "TransferFunction", ptproc, false)) < 0
        )
        return (code < 0 ? code : gs_error_undefined);
    psp->transfer = (code > 0 ? (gs_mapping_proc) 0 : gs_mapped_transfer);
    psp->transfer_closure.proc = 0;
    psp->transfer_closure.data = 0;
    return 0;
}

/* Set actual frequency and angle in a dictionary. */
static int
dict_real_result(i_ctx_t *i_ctx_p, ref * pdict, const char *kstr, double val)
{
    int code = 0;
    ref *ignore;

    if (dict_find_string(pdict, kstr, &ignore) > 0) {
        ref rval;

        check_dict_write(*pdict);
        make_real(&rval, val);
        code = idict_put_string(pdict, kstr, &rval);
    }
    return code;
}
static int
dict_spot_results(i_ctx_t *i_ctx_p, ref * pdict, const gs_spot_halftone * psp)
{
    int code;

    code = dict_real_result(i_ctx_p, pdict, "ActualFrequency",
                            psp->screen.actual_frequency);
    if (code < 0)
        return code;
    return dict_real_result(i_ctx_p, pdict, "ActualAngle",
                            psp->screen.actual_angle);
}

/* Extract Width, Height, and TransferFunction from a dictionary. */
static int
dict_threshold_common_params(const ref * pdict,
                             gs_threshold_halftone_common * ptp,
                             ref **pptstring, ref *ptproc)
{
    int code;

    check_dict_read(*pdict);
    if ((code = dict_int_param(pdict, "Width", 1, 0x7fff, -1,
                               &ptp->width)) < 0 ||
        (code = dict_int_param(pdict, "Height", 1, 0x7fff, -1,
                               &ptp->height)) < 0 ||
        (code = dict_find_string(pdict, "Thresholds", pptstring)) <= 0 ||
      (code = dict_proc_param(pdict, "TransferFunction", ptproc, false)) < 0
        )
        return (code < 0 ? code : gs_error_undefined);
    ptp->transfer_closure.proc = 0;
    ptp->transfer_closure.data = 0;
    return code;
}

/* Extract threshold common parameters + Thresholds. */
static int
dict_threshold_params(const ref * pdict, gs_threshold_halftone * ptp,
                      ref * ptproc)
{
    ref *tstring;
    int code =
        dict_threshold_common_params(pdict,
                                     (gs_threshold_halftone_common *)ptp,
                                     &tstring, ptproc);

    if (code < 0)
        return code;
    check_read_type_only(*tstring, t_string);
    if (r_size(tstring) != (long)ptp->width * ptp->height)
        return_error(gs_error_rangecheck);
    ptp->thresholds.data = tstring->value.const_bytes;
    ptp->thresholds.size = r_size(tstring);
    ptp->transfer = (code > 0 ? (gs_mapping_proc) 0 : gs_mapped_transfer);
    return 0;
}

/* Extract threshold common parameters + Thresholds, Width2, Height2, */
/* BitsPerSample. */
static int
dict_threshold2_params(const ref * pdict, gs_threshold2_halftone * ptp,
                       ref * ptproc, gs_memory_t *mem)
{
    ref *tstring;
    int code =
        dict_threshold_common_params(pdict,
                                     (gs_threshold_halftone_common *)ptp,
                                     &tstring, ptproc);
    int bps;
    uint size;
    int cw2, ch2;

    ptp->transfer = (code > 0 ? (gs_mapping_proc) 0 : gs_mapped_transfer);

    if (code < 0 ||
        (code = cw2 = dict_int_param(pdict, "Width2", 0, 0x7fff, 0,
                                     &ptp->width2)) < 0 ||
        (code = ch2 = dict_int_param(pdict, "Height2", 0, 0x7fff, 0,
                                     &ptp->height2)) < 0 ||
        (code = dict_int_param(pdict, "BitsPerSample", 8, 16, -1, &bps)) < 0
        )
        return code;
    if ((bps != 8 && bps != 16) || cw2 != ch2 ||
        (!cw2 && (ptp->width2 == 0 || ptp->height2 == 0))
        )
        return_error(gs_error_rangecheck);
    ptp->bytes_per_sample = bps / 8;
    switch (r_type(tstring)) {
    case t_string:
        size = r_size(tstring);
        gs_bytestring_from_string(&ptp->thresholds, tstring->value.const_bytes,
                                  size);
        break;
    case t_astruct:
        if (gs_object_type(mem, tstring->value.pstruct) != &st_bytes)
            return_error(gs_error_typecheck);
        size = gs_object_size(mem, tstring->value.pstruct);
        gs_bytestring_from_bytes(&ptp->thresholds, r_ptr(tstring, byte),
                                 0, size);
        break;
    default:
        return_error(gs_error_typecheck);
    }
    check_read(*tstring);
    if (size != (ptp->width * ptp->height + ptp->width2 * ptp->height2) *
        ptp->bytes_per_sample)
        return_error(gs_error_rangecheck);
    return 0;
}
