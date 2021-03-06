/*
 Copyright (C) Intel Corp.  2006.  All Rights Reserved.
 Intel funded Tungsten Graphics to
 develop this 3D driver.

 Permission is hereby granted, free of charge, to any person obtaining
 a copy of this software and associated documentation files (the
 "Software"), to deal in the Software without restriction, including
 without limitation the rights to use, copy, modify, merge, publish,
 distribute, sublicense, and/or sell copies of the Software, and to
 permit persons to whom the Software is furnished to do so, subject to
 the following conditions:

 The above copyright notice and this permission notice (including the
 next paragraph) shall be included in all copies or substantial
 portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 IN NO EVENT SHALL THE COPYRIGHT OWNER(S) AND/OR ITS SUPPLIERS BE
 LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

 **********************************************************************/
 /*
  * Authors:
  *   Keith Whitwell <keithw@vmware.com>
  */



#include "main/mtypes.h"
#include "main/macros.h"
#include "main/fbobject.h"
#include "main/viewport.h"
#include "brw_context.h"
#include "brw_state.h"
#include "brw_defines.h"
#include "brw_sf.h"

static void upload_sf_vp(struct brw_context *brw)
{
   struct gl_context *ctx = &brw->ctx;
   struct brw_sf_viewport *sfv;
   GLfloat y_scale, y_bias;
   float scale[3], translate[3];
   const bool render_to_fbo = _mesa_is_user_fbo(ctx->DrawBuffer);

   sfv = brw_state_batch(brw, AUB_TRACE_SF_VP_STATE,
			 sizeof(*sfv), 32, &brw->sf.vp_offset);
   memset(sfv, 0, sizeof(*sfv));

   /* Accessing the fields Width and Height of gl_framebuffer to produce the
    * values to program the viewport and scissor is fine as long as the
    * gl_framebuffer has atleast one attachment.
    */
   assert(ctx->DrawBuffer->_HasAttachments);

   if (render_to_fbo) {
      y_scale = 1.0;
      y_bias = 0;
   }
   else {
      y_scale = -1.0;
      y_bias = ctx->DrawBuffer->Height;
   }

   /* _NEW_VIEWPORT */

   _mesa_get_viewport_xform(ctx, 0, scale, translate);
   sfv->viewport.m00 = scale[0];
   sfv->viewport.m11 = scale[1] * y_scale;
   sfv->viewport.m22 = scale[2];
   sfv->viewport.m30 = translate[0];
   sfv->viewport.m31 = translate[1] * y_scale + y_bias;
   sfv->viewport.m32 = translate[2];

   /* _NEW_SCISSOR | _NEW_BUFFERS | _NEW_VIEWPORT
    * for DrawBuffer->_[XY]{min,max}
    */

   /* The scissor only needs to handle the intersection of drawable
    * and scissor rect, since there are no longer cliprects for shared
    * buffers with DRI2.
    *
    * Note that the hardware's coordinates are inclusive, while Mesa's min is
    * inclusive but max is exclusive.
    */

   if (ctx->DrawBuffer->_Xmin == ctx->DrawBuffer->_Xmax ||
       ctx->DrawBuffer->_Ymin == ctx->DrawBuffer->_Ymax) {
      /* If the scissor was out of bounds and got clamped to 0
       * width/height at the bounds, the subtraction of 1 from
       * maximums could produce a negative number and thus not clip
       * anything.  Instead, just provide a min > max scissor inside
       * the bounds, which produces the expected no rendering.
       */
      sfv->scissor.xmin = 1;
      sfv->scissor.xmax = 0;
      sfv->scissor.ymin = 1;
      sfv->scissor.ymax = 0;
   } else if (render_to_fbo) {
      /* texmemory: Y=0=bottom */
      sfv->scissor.xmin = ctx->DrawBuffer->_Xmin;
      sfv->scissor.xmax = ctx->DrawBuffer->_Xmax - 1;
      sfv->scissor.ymin = ctx->DrawBuffer->_Ymin;
      sfv->scissor.ymax = ctx->DrawBuffer->_Ymax - 1;
   }
   else {
      /* memory: Y=0=top */
      sfv->scissor.xmin = ctx->DrawBuffer->_Xmin;
      sfv->scissor.xmax = ctx->DrawBuffer->_Xmax - 1;
      sfv->scissor.ymin = ctx->DrawBuffer->Height - ctx->DrawBuffer->_Ymax;
      sfv->scissor.ymax = ctx->DrawBuffer->Height - ctx->DrawBuffer->_Ymin - 1;
   }

   brw->ctx.NewDriverState |= BRW_NEW_SF_VP;
}

const struct brw_tracked_state brw_sf_vp = {
   .dirty = {
      .mesa  = _NEW_BUFFERS |
               _NEW_SCISSOR |
               _NEW_VIEWPORT,
      .brw   = BRW_NEW_BATCH |
               BRW_NEW_BLORP,
   },
   .emit = upload_sf_vp
};

static void upload_sf_unit( struct brw_context *brw )
{
   struct gl_context *ctx = &brw->ctx;
   struct brw_sf_unit_state *sf;
   drm_intel_bo *bo = brw->batch.bo;
   int chipset_max_threads;
   bool render_to_fbo = _mesa_is_user_fbo(ctx->DrawBuffer);

   sf = brw_state_batch(brw, AUB_TRACE_SF_STATE,
			sizeof(*sf), 64, &brw->sf.state_offset);

   memset(sf, 0, sizeof(*sf));

   /* BRW_NEW_PROGRAM_CACHE | BRW_NEW_SF_PROG_DATA */
   sf->thread0.grf_reg_count = ALIGN(brw->sf.prog_data->total_grf, 16) / 16 - 1;
   sf->thread0.kernel_start_pointer =
      brw_program_reloc(brw,
			brw->sf.state_offset +
			offsetof(struct brw_sf_unit_state, thread0),
			brw->sf.prog_offset +
			(sf->thread0.grf_reg_count << 1)) >> 6;

   sf->thread1.floating_point_mode = BRW_FLOATING_POINT_NON_IEEE_754;

   sf->thread3.dispatch_grf_start_reg = 3;
   sf->thread3.urb_entry_read_offset = BRW_SF_URB_ENTRY_READ_OFFSET;

   /* BRW_NEW_SF_PROG_DATA */
   sf->thread3.urb_entry_read_length = brw->sf.prog_data->urb_read_length;

   /* BRW_NEW_URB_FENCE */
   sf->thread4.nr_urb_entries = brw->urb.nr_sf_entries;
   sf->thread4.urb_entry_allocation_size = brw->urb.sfsize - 1;

   /* Each SF thread produces 1 PUE, and there can be up to 24 (Pre-Ironlake) or
    * 48 (Ironlake) threads.
    */
   if (brw->gen == 5)
      chipset_max_threads = 48;
   else
      chipset_max_threads = 24;

   /* BRW_NEW_URB_FENCE */
   sf->thread4.max_threads = MIN2(chipset_max_threads,
				  brw->urb.nr_sf_entries) - 1;

   if (unlikely(INTEL_DEBUG & DEBUG_STATS))
      sf->thread4.stats_enable = 1;

   /* BRW_NEW_SF_VP */
   sf->sf5.sf_viewport_state_offset = (brw->batch.bo->offset64 +
				       brw->sf.vp_offset) >> 5; /* reloc */

   sf->sf5.viewport_transform = 1;

   /* _NEW_SCISSOR */
   if (ctx->Scissor.EnableFlags)
      sf->sf6.scissor = 1;

   /* _NEW_POLYGON */
   if (ctx->Polygon._FrontBit)
      sf->sf5.front_winding = BRW_FRONTWINDING_CW;
   else
      sf->sf5.front_winding = BRW_FRONTWINDING_CCW;

   /* _NEW_BUFFERS
    * The viewport is inverted for rendering to a FBO, and that inverts
    * polygon front/back orientation.
    */
   sf->sf5.front_winding ^= render_to_fbo;

   /* _NEW_POLYGON */
   switch (ctx->Polygon.CullFlag ? ctx->Polygon.CullFaceMode : GL_NONE) {
   case GL_FRONT:
      sf->sf6.cull_mode = BRW_CULLMODE_FRONT;
      break;
   case GL_BACK:
      sf->sf6.cull_mode = BRW_CULLMODE_BACK;
      break;
   case GL_FRONT_AND_BACK:
      sf->sf6.cull_mode = BRW_CULLMODE_BOTH;
      break;
   case GL_NONE:
      sf->sf6.cull_mode = BRW_CULLMODE_NONE;
      break;
   default:
      unreachable("not reached");
   }

   /* _NEW_LINE */
   sf->sf6.line_width =
      CLAMP(ctx->Line.Width, 1.0f, ctx->Const.MaxLineWidth) * (1<<1);

   sf->sf6.line_endcap_aa_region_width = 1;
   if (ctx->Line.SmoothFlag)
      sf->sf6.aa_enable = 1;
   else if (sf->sf6.line_width <= 0x2)
       sf->sf6.line_width = 0;

   /* _NEW_BUFFERS */
   if (!render_to_fbo) {
      /* Rendering to an OpenGL window */
      sf->sf6.point_rast_rule = BRW_RASTRULE_UPPER_RIGHT;
   }
   else {
      /* If rendering to an FBO, the pixel coordinate system is
       * inverted with respect to the normal OpenGL coordinate
       * system, so BRW_RASTRULE_LOWER_RIGHT is correct.
       * But this value is listed as "Reserved, but not seen as useful"
       * in Intel documentation (page 212, "Point Rasterization Rule",
       * section 7.4 "SF Pipeline State Summary", of document
       * "Intel?? 965 Express Chipset Family and Intel?? G35 Express
       * Chipset Graphics Controller Programmer's Reference Manual,
       * Volume 2: 3D/Media", Revision 1.0b as of January 2008,
       * available at
       *     https://01.org/linuxgraphics/documentation/hardware-specification-prms
       * at the time of this writing).
       *
       * It does work on at least some devices, if not all;
       * if devices that don't support it can be identified,
       * the likely failure case is that points are rasterized
       * incorrectly, which is no worse than occurs without
       * the value, so we're using it here.
       */
      sf->sf6.point_rast_rule = BRW_RASTRULE_LOWER_RIGHT;
   }
   /* XXX clamp max depends on AA vs. non-AA */

   /* _NEW_POINT */
   sf->sf7.sprite_point = ctx->Point.PointSprite;
   sf->sf7.point_size = CLAMP(rintf(CLAMP(ctx->Point.Size,
                                          ctx->Point.MinSize,
                                          ctx->Point.MaxSize)), 1.0f, 255.0f) *
                        (1<<3);
   /* _NEW_PROGRAM | _NEW_POINT */
   sf->sf7.use_point_size_state = !(ctx->VertexProgram.PointSizeEnabled ||
				    ctx->Point._Attenuated);
   sf->sf7.aa_line_distance_mode = 0;

   /* might be BRW_NEW_PRIMITIVE if we have to adjust pv for polygons:
    * _NEW_LIGHT
    */
   if (ctx->Light.ProvokingVertex != GL_FIRST_VERTEX_CONVENTION) {
      sf->sf7.trifan_pv = 2;
      sf->sf7.linestrip_pv = 1;
      sf->sf7.tristrip_pv = 2;
   } else {
      sf->sf7.trifan_pv = 1;
      sf->sf7.linestrip_pv = 0;
      sf->sf7.tristrip_pv = 0;
   }
   sf->sf7.line_last_pixel_enable = 0;

   /* Set bias for OpenGL rasterization rules:
    */
   sf->sf6.dest_org_vbias = 0x8;
   sf->sf6.dest_org_hbias = 0x8;

   /* STATE_PREFETCH command description describes this state as being
    * something loaded through the GPE (L2 ISC), so it's INSTRUCTION domain.
    */

   /* Emit SF viewport relocation */
   drm_intel_bo_emit_reloc(bo, (brw->sf.state_offset +
				offsetof(struct brw_sf_unit_state, sf5)),
			   brw->batch.bo, (brw->sf.vp_offset |
					     sf->sf5.front_winding |
					     (sf->sf5.viewport_transform << 1)),
			   I915_GEM_DOMAIN_INSTRUCTION, 0);

   brw->ctx.NewDriverState |= BRW_NEW_GEN4_UNIT_STATE;
}

const struct brw_tracked_state brw_sf_unit = {
   .dirty = {
      .mesa  = _NEW_BUFFERS |
               _NEW_LIGHT |
               _NEW_LINE |
               _NEW_POINT |
               _NEW_POLYGON |
               _NEW_PROGRAM |
               _NEW_SCISSOR,
      .brw   = BRW_NEW_BATCH |
               BRW_NEW_BLORP |
               BRW_NEW_PROGRAM_CACHE |
               BRW_NEW_SF_PROG_DATA |
               BRW_NEW_SF_VP |
               BRW_NEW_URB_FENCE,
   },
   .emit = upload_sf_unit,
};
