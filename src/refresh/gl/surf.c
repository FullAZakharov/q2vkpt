/*
Copyright (C) 2003-2006 Andrey Nazarov

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

/*
 * gl_surf.c -- surface post-processing code
 *
 */
#include "gl.h"

lightmap_builder_t lm;

/*
=============================================================================

LIGHTMAP COLOR ADJUSTING

=============================================================================
*/

static inline void
adjust_color_f(vec_t *out, const vec_t *in, float modulate)
{
    float r, g, b, y, max;

    // add & modulate
    r = (in[0] + gl_static.world.add) * modulate;
    g = (in[1] + gl_static.world.add) * modulate;
    b = (in[2] + gl_static.world.add) * modulate;

    // catch negative lights
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;

    // determine the brightest of the three color components
    max = g;
    if (r > max) {
        max = r;
    }
    if (b > max) {
        max = b;
    }

    // rescale all the color components if the intensity of the greatest
    // channel exceeds 1.0
    if (max > 255) {
        y = 255.0f / max;
        r *= y;
        g *= y;
        b *= y;
    }

    // transform to grayscale by replacing color components with
    // overall pixel luminance computed from weighted color sum
    if (gl_static.world.scale != 1) {
        y = LUMINANCE(r, g, b);
        r = y + (r - y) * gl_static.world.scale;
        g = y + (g - y) * gl_static.world.scale;
        b = y + (b - y) * gl_static.world.scale;
    }

    out[0] = r;
    out[1] = g;
    out[2] = b;
}

static inline void
adjust_color_ub(byte *out, const vec_t *in)
{
    vec3_t tmp;

    adjust_color_f(tmp, in, gl_static.world.modulate);
    out[0] = (byte)tmp[0];
    out[1] = (byte)tmp[1];
    out[2] = (byte)tmp[2];
    out[3] = 255;
}

void GL_AdjustColor(vec3_t color)
{
    adjust_color_f(color, color, gl_static.entity_modulate);
    VectorScale(color, (1.0f / 255), color);
}

/*
=============================================================================

DYNAMIC BLOCKLIGHTS

=============================================================================
*/

#define MAX_SURFACE_EXTENTS     2048
#define MAX_LIGHTMAP_EXTENTS    ((MAX_SURFACE_EXTENTS >> 4) + 1)
#define MAX_BLOCKLIGHTS         (MAX_LIGHTMAP_EXTENTS * MAX_LIGHTMAP_EXTENTS)

static float blocklights[MAX_BLOCKLIGHTS * 3];

#if USE_DLIGHTS
static void add_dynamic_lights(mface_t *surf)
{
    dlight_t *light;
    mtexinfo_t *texinfo;
    cplane_t *plane;
    vec3_t point;
    int local[2];
    vec_t dist, radius, scale, f;
    float *bl;
    int smax, tmax, s, t, sd, td;
    int i, j, k;

    smax = S_MAX(surf);
    tmax = T_MAX(surf);

    k = !!gl_dlight_falloff->integer;
    scale = 1 + 0.1f * k;

    for (i = 0; i < glr.fd.num_dlights; i++) {
        if (!(surf->dlightbits & (1 << i))) {
            continue;
        }

        light = &glr.fd.dlights[i];
        plane = surf->plane;
        dist = PlaneDiffFast(light->transformed, plane);
        radius = light->intensity * scale - fabs(dist);
        if (radius < DLIGHT_CUTOFF) {
            continue;
        }

        VectorMA(light->transformed, -dist, plane->normal, point);

        texinfo = surf->texinfo;
        local[0] = DotProduct(point, texinfo->axis[0]) + texinfo->offset[0];
        local[1] = DotProduct(point, texinfo->axis[1]) + texinfo->offset[1];

        local[0] -= surf->texturemins[0];
        local[1] -= surf->texturemins[1];

        bl = blocklights;
        for (t = 0; t < tmax; t++) {
            td = abs(local[1] - (t << 4));
            for (s = 0; s < smax; s++) {
                sd = abs(local[0] - (s << 4));
                if (sd > td) {
                    j = sd + (td >> 1);
                } else {
                    j = td + (sd >> 1);
                }

                if (j + DLIGHT_CUTOFF < radius) {
                    f = radius - (j + DLIGHT_CUTOFF * k);
                    bl[0] += light->color[0] * f;
                    bl[1] += light->color[1] * f;
                    bl[2] += light->color[2] * f;
                }

                bl += 3;
            }
        }
    }
}
#endif

static void add_light_styles(mface_t *surf, int size)
{
    lightstyle_t *style;
    byte *src;
    float *bl;
    int i, j;

    if (!surf->numstyles) {
        // should this ever happen?
        memset(blocklights, 0, sizeof(blocklights[0]) * size * 3);
        return;
    }

    // init primary lightmap
    style = LIGHT_STYLE(surf, 0);

    src = surf->lightmap;
    bl = blocklights;
    if (style->white == 1) {
        for (j = 0; j < size; j++) {
            bl[0] = src[0];
            bl[1] = src[1];
            bl[2] = src[2];

            bl += 3; src += 3;
        }
    } else {
        for (j = 0; j < size; j++) {
            bl[0] = src[0] * style->rgb[0];
            bl[1] = src[1] * style->rgb[1];
            bl[2] = src[2] * style->rgb[2];

            bl += 3; src += 3;
        }
    }

    surf->stylecache[0] = style->white;

    // add remaining lightmaps
    for (i = 1; i < surf->numstyles; i++) {
        style = LIGHT_STYLE(surf, i);

        bl = blocklights;
        for (j = 0; j < size; j++) {
            bl[0] += src[0] * style->rgb[0];
            bl[1] += src[1] * style->rgb[1];
            bl[2] += src[2] * style->rgb[2];

            bl += 3; src += 3;
        }

        surf->stylecache[i] = style->white;
    }
}

static void update_dynamic_lightmap(mface_t *surf)
{
    byte temp[MAX_BLOCKLIGHTS * 4], *dst;
    int smax, tmax, size, i;
    float *bl;

    smax = S_MAX(surf);
    tmax = T_MAX(surf);
    size = smax * tmax;

    // add all the lightmaps
    add_light_styles(surf, size);

#if USE_DLIGHTS
    // add all the dynamic lights
    if (surf->dlightframe == glr.dlightframe) {
        add_dynamic_lights(surf);
    } else {
        surf->dlightframe = 0;
    }
#endif

    // put into texture format
    bl = blocklights;
    dst = temp;
    for (i = 0; i < size; i++) {
        adjust_color_ub(dst, bl);
        bl += 3; dst += 4;
    }

    // upload lightmap subimage
    GL_BindTexture(surf->texnum[1]);
    qglTexSubImage2D(GL_TEXTURE_2D, 0,
                     surf->light_s, surf->light_t, smax, tmax,
                     GL_RGBA, GL_UNSIGNED_BYTE, temp);

    c.texUploads++;
}

void GL_BeginLights(void)
{
    qglActiveTextureARB(GL_TEXTURE1_ARB);
    gls.tmu = 1;
}

void GL_EndLights(void)
{
    qglActiveTextureARB(GL_TEXTURE0_ARB);
    gls.tmu = 0;
}

void GL_PushLights(mface_t *surf)
{
    lightstyle_t *style;
    int i;

#if USE_DLIGHTS
    // dynamic this frame or dynamic previously
    if (surf->dlightframe) {
        update_dynamic_lightmap(surf);
        return;
    }
#endif

    // check for light style updates
    for (i = 0; i < surf->numstyles; i++) {
        style = LIGHT_STYLE(surf, i);
        if (style->white != surf->stylecache[i]) {
            update_dynamic_lightmap(surf);
            return;
        }
    }
}

/*
=============================================================================

LIGHTMAPS BUILDING

=============================================================================
*/

#define LM_AllocBlock(w, h, s, t) \
    GL_AllocBlock(LM_BLOCK_WIDTH, LM_BLOCK_HEIGHT, lm.inuse, w, h, s, t)

static void LM_InitBlock(void)
{
    int i;

    for (i = 0; i < LM_BLOCK_WIDTH; i++) {
        lm.inuse[i] = 0;
    }

    lm.dirty = qfalse;
}

static void LM_UploadBlock(void)
{
    if (!lm.dirty) {
        return;
    }

    // bypassing our state tracker here, be careful to reset TMU1 afterwards!
    qglBindTexture(GL_TEXTURE_2D, TEXNUM_LIGHTMAP + lm.nummaps);
    qglTexImage2D(GL_TEXTURE_2D, 0, lm.comp, LM_BLOCK_WIDTH, LM_BLOCK_HEIGHT, 0,
                  GL_RGBA, GL_UNSIGNED_BYTE, lm.buffer);
    qglTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    qglTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    if (lm.highwater < ++lm.nummaps) {
        lm.highwater = lm.nummaps;
    }
}

static void build_style_map(int dynamic)
{
    static lightstyle_t fake;
    int i;

    if (!dynamic) {
        // make all styles fullbright
        fake.rgb[0] = 1;
        fake.rgb[1] = 1;
        fake.rgb[2] = 1;
        fake.white = 1;
        glr.fd.lightstyles = &fake;

        memset(gl_static.lightstylemap, 0, sizeof(gl_static.lightstylemap));
        return;
    }

    for (i = 0; i < MAX_LIGHTSTYLES; i++) {
        gl_static.lightstylemap[i] = i;
    }

    if (dynamic != 1) {
        // make dynamic styles fullbright
        for (i = 1; i < 32; i++) {
            gl_static.lightstylemap[i] = 0;
        }
    }
}

static void LM_BeginBuilding(void)
{
    qglActiveTextureARB(GL_TEXTURE1_ARB);
    LM_InitBlock();

    // start up with fullbright styles
    build_style_map(0);
}

static void LM_EndBuilding(void)
{
    // upload the last lightmap
    LM_UploadBlock();
    LM_InitBlock();

    qglActiveTextureARB(GL_TEXTURE0_ARB);

    // because LM_UploadBlock doesn't use our state tracker functions,
    // their idea of what is bound to TMU1 needs to be reset
    gls.texnum[1] = 0;

    // now build the real lightstyle map
    build_style_map(gl_dynamic->integer);
}

static void LM_FreeLightmaps(void)
{
    // lightmap textures are not deleted from memory when changing maps,
    // they are merely reused
    lm.nummaps = 0;
}

static void build_primary_lightmap(mface_t *surf)
{
    byte *ptr, *dst;
    int smax, tmax, size, i, j;
    float *bl;

    smax = S_MAX(surf);
    tmax = T_MAX(surf);
    size = smax * tmax;

    // add all the lightmaps
    add_light_styles(surf, size);

#if USE_DLIGHTS
    surf->dlightframe = 0;
#endif

    // put into texture format
    bl = blocklights;
    dst = &lm.buffer[(surf->light_t * LM_BLOCK_WIDTH + surf->light_s) << 2];
    for (i = 0; i < tmax; i++) {
        ptr = dst;
        for (j = 0; j < smax; j++) {
            adjust_color_ub(ptr, bl);
            bl += 3; ptr += 4;
        }

        dst += LM_BLOCK_WIDTH * 4;
    }
}

static qboolean LM_BuildSurface(mface_t *surf, vec_t *vbo)
{
    int smax, tmax, size, s, t, i;
    byte *src, *ptr;
    bsp_t *bsp;

    // validate extents
    if (surf->extents[0] < 0 || surf->extents[0] > MAX_SURFACE_EXTENTS ||
        surf->extents[1] < 0 || surf->extents[1] > MAX_SURFACE_EXTENTS) {
        Com_EPrintf("%s: bad surface extents\n", __func__);
        return qfalse;
    }

    // validate blocklights size
    smax = S_MAX(surf);
    tmax = T_MAX(surf);
    size = smax * tmax;
    if (size > MAX_BLOCKLIGHTS) {
        Com_EPrintf("%s: MAX_BLOCKLIGHTS exceeded\n", __func__);
        return qfalse;
    }

    // validate lightmap bounds
    bsp = gl_static.world.cache;
    src = surf->lightmap + surf->numstyles * size * 3;
    ptr = bsp->lightmap + bsp->numlightmapbytes;
    if (src > ptr) {
        Com_EPrintf("%s: bad surface lightmap\n", __func__);
        return qfalse;
    }

    if (!LM_AllocBlock(smax, tmax, &s, &t)) {
        LM_UploadBlock();
        if (lm.nummaps == LM_MAX_LIGHTMAPS) {
            Com_EPrintf("%s: LM_MAX_LIGHTMAPS exceeded\n", __func__);
            return qfalse;
        }
        LM_InitBlock();
        if (!LM_AllocBlock(smax, tmax, &s, &t)) {
            Com_EPrintf("%s: LM_AllocBlock(%d, %d) failed\n",
                        __func__, smax, tmax);
            return qfalse;
        }
    }

    lm.dirty = qtrue;

    // store the surface lightmap parameters
    surf->light_s = s;
    surf->light_t = t;
    surf->texnum[1] = TEXNUM_LIGHTMAP + lm.nummaps;

    // build the primary lightmap
    build_primary_lightmap(surf);

    // normalize and store lmtc in vertices
    s = (s << 4) + 8;
    t = (t << 4) + 8;

    s -= surf->texturemins[0];
    t -= surf->texturemins[1];

    for (i = 0; i < surf->numsurfedges; i++) {
        vbo[5] += s;
        vbo[6] += t;
        vbo[5] /= LM_BLOCK_WIDTH * 16;
        vbo[6] /= LM_BLOCK_HEIGHT * 16;
        vbo += VERTEX_SIZE;
    }

    return qtrue;
}

// called from the main loop whenever lightmap parameters change
void LM_RebuildSurfaces(void)
{
    bsp_t *bsp = gl_static.world.cache;
    mface_t *surf;
    int i, texnum;

    if (!bsp) {
        return;
    }

    build_style_map(gl_dynamic->integer);

    if (!lm.nummaps) {
        return;
    }

    qglActiveTextureARB(GL_TEXTURE1_ARB);
    qglBindTexture(GL_TEXTURE_2D, TEXNUM_LIGHTMAP);
    texnum = TEXNUM_LIGHTMAP;

    for (i = 0, surf = bsp->faces; i < bsp->numfaces; i++, surf++) {
        if (!surf->lightmap) {
            continue;
        }
        if (surf->drawflags & SURF_NOLM_MASK) {
            continue;
        }
        if (!surf->texnum[1]) {
            continue;
        }

        if (surf->texnum[1] != texnum) {
            // done with previous lightmap
            qglTexImage2D(GL_TEXTURE_2D, 0, lm.comp,
                          LM_BLOCK_WIDTH, LM_BLOCK_HEIGHT, 0,
                          GL_RGBA, GL_UNSIGNED_BYTE, lm.buffer);
            qglBindTexture(GL_TEXTURE_2D, surf->texnum[1]);
            texnum = surf->texnum[1];

            c.texUploads++;
        }

        build_primary_lightmap(surf);
    }

    // upload the last lightmap
    qglTexImage2D(GL_TEXTURE_2D, 0, lm.comp,
                  LM_BLOCK_WIDTH, LM_BLOCK_HEIGHT, 0,
                  GL_RGBA, GL_UNSIGNED_BYTE, lm.buffer);

    c.texUploads++;

    qglActiveTextureARB(GL_TEXTURE0_ARB);
    gls.texnum[1] = 0;
}


/*
=============================================================================

POLYGONS BUILDING

=============================================================================
*/

static void build_surface_poly(mface_t *surf, vec_t *vbo)
{
    msurfedge_t *src_surfedge;
    mvertex_t *src_vert;
    medge_t *src_edge;
    mtexinfo_t *texinfo = surf->texinfo;
    int i;
    vec2_t scale, tc, mins, maxs;
    int bmins[2], bmaxs[2];

    surf->texnum[0] = texinfo->image->texnum;
    surf->texnum[1] = 0;

    // normalize texture coordinates
    scale[0] = 1.0f / texinfo->image->width;
    scale[1] = 1.0f / texinfo->image->height;

    mins[0] = mins[1] = 99999;
    maxs[0] = maxs[1] = -99999;

    src_surfedge = surf->firstsurfedge;
    for (i = 0; i < surf->numsurfedges; i++) {
        src_edge = src_surfedge->edge;
        src_vert = src_edge->v[src_surfedge->vert];
        src_surfedge++;

        // vertex coordinates
        VectorCopy(src_vert->point, vbo);

        // texture0 coordinates
        tc[0] = DotProduct(vbo, texinfo->axis[0]) + texinfo->offset[0];
        tc[1] = DotProduct(vbo, texinfo->axis[1]) + texinfo->offset[1];

        if (mins[0] > tc[0]) mins[0] = tc[0];
        if (maxs[0] < tc[0]) maxs[0] = tc[0];

        if (mins[1] > tc[1]) mins[1] = tc[1];
        if (maxs[1] < tc[1]) maxs[1] = tc[1];

        vbo[3] = tc[0] * scale[0];
        vbo[4] = tc[1] * scale[1];

        // texture1 coordinates
        vbo[5] = tc[0];
        vbo[6] = tc[1];

        vbo += VERTEX_SIZE;
    }

    // calculate surface extents
    bmins[0] = floor(mins[0] / 16);
    bmins[1] = floor(mins[1] / 16);
    bmaxs[0] = ceil(maxs[0] / 16);
    bmaxs[1] = ceil(maxs[1] / 16);

    surf->texturemins[0] = bmins[0] << 4;
    surf->texturemins[1] = bmins[1] << 4;

    surf->extents[0] = (bmaxs[0] - bmins[0]) << 4;
    surf->extents[1] = (bmaxs[1] - bmins[1]) << 4;
}

// duplicates normalized texture0 coordinates for non-lit surfaces in texture1
// to make them render properly when gl_lightmap hack is used
static void duplicate_surface_lmtc(mface_t *surf, vec_t *vbo)
{
    int i;

    for (i = 0; i < surf->numsurfedges; i++) {
        vbo[5] = vbo[3];
        vbo[6] = vbo[4];

        vbo += VERTEX_SIZE;
    }
}

void GL_FreeWorld(void)
{
    if (!gl_static.world.cache) {
        return;
    }

    BSP_Free(gl_static.world.cache);

    if (gl_static.world.vertices) {
        Hunk_Free(&gl_static.world.hunk);
    } else if (qglDeleteBuffersARB) {
        qglDeleteBuffersARB(1, &gl_static.world.bufnum);
    }

    LM_FreeLightmaps();

    memset(&gl_static.world, 0, sizeof(gl_static.world));
}

static qboolean create_surface_vbo(size_t size)
{
    GLuint buf;

    if (!qglGenBuffersARB || !qglBindBufferARB ||
        !qglBufferDataARB || !qglBufferSubDataARB ||
        !qglDeleteBuffersARB) {
        return qfalse;
    }

    GL_ClearErrors();

    qglGenBuffersARB(1, &buf);
    qglBindBufferARB(GL_ARRAY_BUFFER_ARB, buf);
    qglBufferDataARB(GL_ARRAY_BUFFER_ARB, size, NULL, GL_STATIC_DRAW_ARB);

    if (GL_ShowErrors("Failed to create world model VBO")) {
        qglBindBufferARB(GL_ARRAY_BUFFER_ARB, 0);
        qglDeleteBuffersARB(1, &buf);
        return qfalse;
    }

    gl_static.world.vertices = NULL;
    gl_static.world.bufnum = buf;
    return qtrue;
}

static void upload_surface_vbo(int lastvert)
{
    GLintptrARB offset = lastvert * VERTEX_SIZE * sizeof(vec_t);
    GLsizeiptrARB size = tess.numverts * VERTEX_SIZE * sizeof(vec_t);

    Com_DDPrintf("%s: %"PRIz" bytes\n", __func__, size);

    qglBufferSubDataARB(GL_ARRAY_BUFFER_ARB, offset, size, tess.vertices);
    tess.numverts = 0;
}

// silence GCC warning
extern void gl_lightmap_changed(cvar_t *self);

void gl_lightmap_changed(cvar_t *self)
{
    gl_static.world.scale = Cvar_ClampValue(gl_coloredlightmaps, 0, 1);
    lm.comp = gl_static.world.scale ? GL_RGB : GL_LUMINANCE;

    // FIXME: the name 'brightness' is misleading in this context
    gl_static.world.add = 255 * Cvar_ClampValue(gl_brightness, -1, 1);

    gl_static.world.modulate = gl_modulate->value * gl_modulate_world->value;

    // rebuild all lightmaps next frame
    lm.dirty = qtrue;
}

void GL_LoadWorld(const char *name)
{
    char buffer[MAX_QPATH];
    mface_t *surf;
    int i, count, lastvert;
    size_t size;
    vec_t s, t;
    vec_t *vbo;
    bsp_t *bsp;
    mtexinfo_t *info;
    qerror_t ret;

    ret = BSP_Load(name, &bsp);
    if (!bsp) {
        Com_Error(ERR_DROP, "%s: couldn't load %s: %s",
                  __func__, name, Q_ErrorString(ret));
    }

    // check if the required world model was already loaded
    if (gl_static.world.cache == bsp) {
        for (i = 0; i < bsp->numtexinfo; i++) {
            bsp->texinfo[i].image->registration_sequence = registration_sequence;
        }
        for (i = 0; i < bsp->numnodes; i++) {
            bsp->nodes[i].visframe = 0;
        }
        for (i = 0; i < bsp->numleafs; i++) {
            bsp->leafs[i].visframe = 0;
        }
        Com_DPrintf("%s: reused old world model\n", __func__);
        bsp->refcount--;
        return;
    }

    // free previous model, if any
    GL_FreeWorld();

    gl_lightmap_changed(NULL);

    gl_static.world.cache = bsp;

    // calculate world size for far clip plane and sky box
    for (i = 0, s = 0; i < 3; i++) {
        t = bsp->nodes[0].maxs[i] - bsp->nodes[0].mins[i];
        if (t > s)
            s = t;
    }

    if (s > 4096)
        gl_static.world.size = 8192;
    else if (s > 2048)
        gl_static.world.size = 4096;
    else
        gl_static.world.size = 2048;

    Com_DPrintf("%s: world size %.f (%.f)\n", __func__, gl_static.world.size, s);

    // register all texinfo
    for (i = 0, info = bsp->texinfo; i < bsp->numtexinfo; i++, info++) {
        Q_concat(buffer, sizeof(buffer), "textures/", info->name, ".wal", NULL);
        FS_NormalizePath(buffer, buffer);
        upload_texinfo = info;
        info->image = IMG_Find(buffer, IT_WALL);
        upload_texinfo = NULL;
    }

    // calculate vertex buffer size in bytes
    count = 0;
    for (i = 0, surf = bsp->faces; i < bsp->numfaces; i++, surf++) {
        if (!(surf->texinfo->c.flags & SURF_SKY)) {
            count += surf->numsurfedges;
        }
    }
    size = count * VERTEX_SIZE * sizeof(vec_t);

    // try VBO first, then allocate on hunk
    if (create_surface_vbo(size)) {
        Com_DPrintf("%s: %"PRIz" bytes of vertex data as VBO\n", __func__, size);
    } else {
        Hunk_Begin(&gl_static.world.hunk, size);
        vbo = Hunk_Alloc(&gl_static.world.hunk, size);
        Hunk_End(&gl_static.world.hunk);

        Com_DPrintf("%s: %"PRIz" bytes of vertex data on hunk\n", __func__, size);
        gl_static.world.vertices = vbo;
    }

    // begin building lightmaps
    LM_BeginBuilding();

    // post process all surfaces
    count = 0;
    lastvert = 0;
    for (i = 0, surf = bsp->faces; i < bsp->numfaces; i++, surf++) {
        // hack surface flags into drawflags for faster access
        surf->drawflags |= surf->texinfo->c.flags & ~DSURF_PLANEBACK;

        if (surf->drawflags & SURF_SKY) {
            continue;
        }

        if (gl_static.world.vertices) {
            vbo = gl_static.world.vertices + count * VERTEX_SIZE;
        } else {
            if (surf->numsurfedges > TESS_MAX_VERTICES) {
                Com_EPrintf("%s: too many verts\n", __func__);
                continue;
            }

            // upload VBO chunk if needed
            if (tess.numverts + surf->numsurfedges > TESS_MAX_VERTICES) {
                upload_surface_vbo(lastvert);
                lastvert = count;
            }

            vbo = tess.vertices + tess.numverts * VERTEX_SIZE;
            tess.numverts += surf->numsurfedges;
        }

        surf->firstvert = count;
        build_surface_poly(surf, vbo);

        if (gl_fullbright->integer || (surf->drawflags & SURF_NOLM_MASK)) {
            surf->lightmap = NULL;
        } else if (surf->lightmap && !LM_BuildSurface(surf, vbo)) {
            surf->lightmap = NULL;
        }

        if (!surf->lightmap) {
            duplicate_surface_lmtc(surf, vbo);
        }

        count += surf->numsurfedges;
    }

    // upload the last VBO chunk
    if (!gl_static.world.vertices) {
        upload_surface_vbo(lastvert);
        qglBindBufferARB(GL_ARRAY_BUFFER_ARB, 0);
    }

    // end building lightmaps
    LM_EndBuilding();
    Com_DPrintf("%s: %d lightmaps built\n", __func__, lm.nummaps);
}

