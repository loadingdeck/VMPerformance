/* c-ray-mt - a simple multithreaded raytracing filter.
 * Copyright (C) 2006 John Tsiombikas <nuclear@siggraph.org>
 *
 * You are free to use, modify and redistribute this program under the
 * terms of the GNU General Public License v2 or (at your option) later.
 * see "http://www.gnu.org/licenses/gpl.txt" for details.
 * ---------------------------------------------------------------------
 * Usage:
 *   compile:  just type make
 *              (add any arch-specific optimizations for your compiler in CFLAGS first)
 *       run:  cat scene | ./c-ray-mt [-t num-threads] >foo.ppm
 *              (on broken systems such as windows try: c-ray-mt -i scene -o foo.ppm)
 *     enjoy:  display foo.ppm
 *              (with imagemagick, or use your favorite image viewer)
 * ---------------------------------------------------------------------
 * Scene file format:
 *   # sphere (many)
 *   s  x y z  rad   r g b   shininess   reflectivity
 *   # light (many)
 *   l  x y z
 *   # camera (one)
 *   c  x y z  fov   tx ty tz
 * ---------------------------------------------------------------------
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <errno.h>
#include <pthread.h>

#include "c-ray.h"

#define VER_MAJOR	1
#define VER_MINOR	1
#define VER_STR		"c-ray-mt v%d.%d\n"

#if !defined(unix) && !defined(__unix__)
#ifdef __MACH__
#define unix		1
#define __unix__	1
#endif	/* __MACH__ */
#endif	/* unix */

/* find the appropriate way to define explicitly sized types */
/* for C99 or GNU libc (also mach's libc) we can use stdint.h */
#if (__STDC_VERSION__ >= 199900) || defined(__GLIBC__) || defined(__MACH__)
#include <stdint.h>
#elif defined(unix) || defined(__unix__)	/* some UNIX systems have them in sys/types.h */
#include <sys/types.h>
#elif defined(__WIN32__) || defined(WIN32)	/* the nameless one */
typedef unsigned __int8 uint8_t;
typedef unsigned __int32 uint32_t;
#endif	/* sized type detection */

struct vec3 {
	double x, y, z;
};

struct ray {
	struct vec3 orig, dir;
};

struct material {
	struct vec3 col;	/* color */
	double spow;		/* specular power */
	double refl;		/* reflection intensity */
};

struct sphere {
	struct vec3 pos;
	double rad;
	struct material mat;
	struct sphere *next;
};

struct spoint {
	struct vec3 pos, normal, vref;	/* position, normal and view reflection */
	double dist;		/* parametric distance of intersection along the ray */
};

struct camera {
	struct vec3 pos, targ;
	double fov;
};

struct thread_data {
	pthread_t tid;
	int sl_start, sl_count;

	uint32_t *pixels;
};

static void render_scanline(int xsz, int ysz, int sl, uint32_t *fb, int samples);
static struct vec3 trace(struct ray ray, int depth);
static struct vec3 shade(struct sphere *obj, struct spoint *sp, int depth);
static struct vec3 reflect(struct vec3 v, struct vec3 n);
static struct vec3 cross_product(struct vec3 v1, struct vec3 v2);
static struct ray get_primary_ray(int x, int y, int sample);
static struct vec3 get_sample_pos(int x, int y, int sample);
static struct vec3 jitter(int x, int y, int s);
static int ray_sphere(const struct sphere *sph, struct ray ray, struct spoint *sp);
static unsigned long get_msec(void);

static void *thread_func(void *tdata);

#define MAX_LIGHTS		16				/* maximum number of lights */
#define RAY_MAG			1000.0			/* trace rays of this magnitude */
#define MAX_RAY_DEPTH	5				/* raytrace recursion limit */
#define FOV				0.78539816		/* field of view in rads (pi/4) */
#define HALF_FOV		(FOV * 0.5)
#define ERR_MARGIN		1e-6			/* an arbitrary error margin to avoid surface acne */

/* bit-shift ammount for packing each color into a 32bit uint */
#ifdef LITTLE_ENDIAN
#define RSHIFT	16
#define BSHIFT	0
#else	/* big endian */
#define RSHIFT	0
#define BSHIFT	16
#endif	/* endianess */
#define GSHIFT	8	/* this is the same in both byte orders */

/* some helpful macros... */
#define SQ(x)		((x) * (x))
#define MAX(a, b)	((a) > (b) ? (a) : (b))
#define MIN(a, b)	((a) < (b) ? (a) : (b))
#define DOT(a, b)	((a).x * (b).x + (a).y * (b).y + (a).z * (b).z)
#define NORMALIZE(a)  do {\
	double len = sqrt(DOT(a, a));\
	(a).x /= len; (a).y /= len; (a).z /= len;\
} while(0);

/* global state */
static int xres = 800;
static int yres = 600;
static int rays_per_pixel = 1;
static double aspect = 1.333333;
static struct sphere *obj_list;
static struct vec3 lights[MAX_LIGHTS];
static int lnum = 0;
static struct camera cam;
static struct thread_data *threads;

static int start = 0;
static pthread_mutex_t start_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t start_cond = PTHREAD_COND_INITIALIZER;

#define NRAN	1024
#define MASK	(NRAN - 1)
static struct vec3 urand[NRAN];
static int irand[NRAN];

/* render a frame of xsz/ysz dimensions into the provided framebuffer */
static void render_scanline(int xsz, int ysz, int sl, uint32_t *fb, int samples) {
	int i, s;
	double rcp_samples = 1.0 / (double)samples;

	for(i=0; i<xsz; i++) {
		double r, g, b;
		r = g = b = 0.0;

		for(s=0; s<samples; s++) {
			struct vec3 col = trace(get_primary_ray(i, sl, s), 0);
			r += col.x;
			g += col.y;
			b += col.z;
		}

		r = r * rcp_samples;
		g = g * rcp_samples;
		b = b * rcp_samples;

		fb[sl * xsz + i] = ((uint32_t)(MIN(r, 1.0) * 255.0) & 0xff) << RSHIFT |
							((uint32_t)(MIN(g, 1.0) * 255.0) & 0xff) << GSHIFT |
							((uint32_t)(MIN(b, 1.0) * 255.0) & 0xff) << BSHIFT;
	}
}

/* trace a ray throught the scene recursively (the recursion happens through
 * shade() to calculate reflection rays if necessary).
 */
static struct vec3 trace(struct ray ray, int depth) {
	struct vec3 col;
	struct spoint sp, nearest_sp = {.pos={0}, .normal={0}, .vref={0}, .dist=0.0f};
	struct sphere *nearest_obj = 0;
	struct sphere *iter = obj_list->next;

	/* if we reached the recursion limit, bail out */
	if(depth >= MAX_RAY_DEPTH) {
		col.x = col.y = col.z = 0.0;
		return col;
	}

	/* find the nearest intersection ... */
	while(iter) {
		if(ray_sphere(iter, ray, &sp)) {
			if(!nearest_obj || sp.dist < nearest_sp.dist) {
				nearest_obj = iter;
				nearest_sp = sp;
			}
		}
		iter = iter->next;
	}

	/* and perform shading calculations as needed by calling shade() */
	if(nearest_obj) {
		col = shade(nearest_obj, &nearest_sp, depth);
	} else {
		col.x = col.y = col.z = 0.0;
	}

	return col;
}

/* Calculates direct illumination with the phong reflectance model.
 * Also handles reflections by calling trace again, if necessary.
 */
static struct vec3 shade(struct sphere *obj, struct spoint *sp, int depth) {
	int i;
	struct vec3 col = {0, 0, 0};

	/* for all lights ... */
	for(i=0; i<lnum; i++) {
		double ispec, idiff;
		struct vec3 ldir;
		struct ray shadow_ray;
		struct sphere *iter = obj_list->next;
		int in_shadow = 0;

		ldir.x = lights[i].x - sp->pos.x;
		ldir.y = lights[i].y - sp->pos.y;
		ldir.z = lights[i].z - sp->pos.z;

		shadow_ray.orig = sp->pos;
		shadow_ray.dir = ldir;

		/* shoot shadow rays to determine if we have a line of sight with the light */
		while(iter) {
			if(ray_sphere(iter, shadow_ray, 0)) {
				in_shadow = 1;
				break;
			}
			iter = iter->next;
		}

		/* and if we're not in shadow, calculate direct illumination with the phong model. */
		if(!in_shadow) {
			NORMALIZE(ldir);

			idiff = MAX(DOT(sp->normal, ldir), 0.0);
			ispec = obj->mat.spow > 0.0 ? pow(MAX(DOT(sp->vref, ldir), 0.0), obj->mat.spow) : 0.0;

			col.x += idiff * obj->mat.col.x + ispec;
			col.y += idiff * obj->mat.col.y + ispec;
			col.z += idiff * obj->mat.col.z + ispec;
		}
	}

	/* Also, if the object is reflective, spawn a reflection ray, and call trace()
	 * to calculate the light arriving from the mirror direction.
	 */
	if(obj->mat.refl > 0.0) {
		struct ray ray;
		struct vec3 rcol;

		ray.orig = sp->pos;
		ray.dir = sp->vref;
		ray.dir.x *= RAY_MAG;
		ray.dir.y *= RAY_MAG;
		ray.dir.z *= RAY_MAG;

		rcol = trace(ray, depth + 1);
		col.x += rcol.x * obj->mat.refl;
		col.y += rcol.y * obj->mat.refl;
		col.z += rcol.z * obj->mat.refl;
	}

	return col;
}

/* calculate reflection vector */
static struct vec3 reflect(struct vec3 v, struct vec3 n) {
	struct vec3 res;
	double dot = v.x * n.x + v.y * n.y + v.z * n.z;
	res.x = -(2.0 * dot * n.x - v.x);
	res.y = -(2.0 * dot * n.y - v.y);
	res.z = -(2.0 * dot * n.z - v.z);
	return res;
}

static struct vec3 cross_product(struct vec3 v1, struct vec3 v2) {
	struct vec3 res;
	res.x = v1.y * v2.z - v1.z * v2.y;
	res.y = v1.z * v2.x - v1.x * v2.z;
	res.z = v1.x * v2.y - v1.y * v2.x;
	return res;
}

/* determine the primary ray corresponding to the specified pixel (x, y) */
static struct ray get_primary_ray(int x, int y, int sample) {
	struct ray ray;
	float m[3][3];
	struct vec3 i, j = {0, 1, 0}, k, dir, orig, foo;

	k.x = cam.targ.x - cam.pos.x;
	k.y = cam.targ.y - cam.pos.y;
	k.z = cam.targ.z - cam.pos.z;
	NORMALIZE(k);

	i = cross_product(j, k);
	j = cross_product(k, i);
	m[0][0] = i.x; m[0][1] = j.x; m[0][2] = k.x;
	m[1][0] = i.y; m[1][1] = j.y; m[1][2] = k.y;
	m[2][0] = i.z; m[2][1] = j.z; m[2][2] = k.z;

	ray.orig.x = ray.orig.y = ray.orig.z = 0.0;
	ray.dir = get_sample_pos(x, y, sample);
	ray.dir.z = 1.0 / HALF_FOV;
	ray.dir.x *= RAY_MAG;
	ray.dir.y *= RAY_MAG;
	ray.dir.z *= RAY_MAG;

	dir.x = ray.dir.x + ray.orig.x;
	dir.y = ray.dir.y + ray.orig.y;
	dir.z = ray.dir.z + ray.orig.z;
	foo.x = dir.x * m[0][0] + dir.y * m[0][1] + dir.z * m[0][2];
	foo.y = dir.x * m[1][0] + dir.y * m[1][1] + dir.z * m[1][2];
	foo.z = dir.x * m[2][0] + dir.y * m[2][1] + dir.z * m[2][2];

	orig.x = ray.orig.x * m[0][0] + ray.orig.y * m[0][1] + ray.orig.z * m[0][2] + cam.pos.x;
	orig.y = ray.orig.x * m[1][0] + ray.orig.y * m[1][1] + ray.orig.z * m[1][2] + cam.pos.y;
	orig.z = ray.orig.x * m[2][0] + ray.orig.y * m[2][1] + ray.orig.z * m[2][2] + cam.pos.z;

	ray.orig = orig;
	ray.dir.x = foo.x + orig.x;
	ray.dir.y = foo.y + orig.y;
	ray.dir.z = foo.z + orig.z;

	return ray;
}


static struct vec3 get_sample_pos(int x, int y, int sample) {
	struct vec3 pt;
	static double sf = 0.0;

	if(sf == 0.0) {
		sf = 1.5 / (double)xres;
	}

	pt.x = ((double)x / (double)xres) - 0.5;
	pt.y = -(((double)y / (double)yres) - 0.65) / aspect;

	if(sample) {
		struct vec3 jt = jitter(x, y, sample);
		pt.x += jt.x * sf;
		pt.y += jt.y * sf / aspect;
	}
	return pt;
}

/* jitter function taken from Graphics Gems I. */
static struct vec3 jitter(int x, int y, int s) {
	struct vec3 pt;
	pt.x = urand[(x + (y << 2) + irand[(x + s) & MASK]) & MASK].x;
	pt.y = urand[(y + (x << 2) + irand[(y + s) & MASK]) & MASK].y;
	return pt;
}

/* Calculate ray-sphere intersection, and return {1, 0} to signify hit or no hit.
 * Also the surface point parameters like position, normal, etc are returned through
 * the sp pointer if it is not NULL.
 */
static  int ray_sphere(const struct sphere *sph, struct ray ray, struct spoint *sp) {
	double a, b, c, d, sqrt_d, t1, t2;

	a = SQ(ray.dir.x) + SQ(ray.dir.y) + SQ(ray.dir.z);
	b = 2.0 * ray.dir.x * (ray.orig.x - sph->pos.x) +
				2.0 * ray.dir.y * (ray.orig.y - sph->pos.y) +
				2.0 * ray.dir.z * (ray.orig.z - sph->pos.z);
	c = SQ(sph->pos.x) + SQ(sph->pos.y) + SQ(sph->pos.z) +
				SQ(ray.orig.x) + SQ(ray.orig.y) + SQ(ray.orig.z) +
				2.0 * (-sph->pos.x * ray.orig.x - sph->pos.y * ray.orig.y - sph->pos.z * ray.orig.z) - SQ(sph->rad);

	if((d = SQ(b) - 4.0 * a * c) < 0.0) return 0;

	sqrt_d = sqrt(d);
	t1 = (-b + sqrt_d) / (2.0 * a);
	t2 = (-b - sqrt_d) / (2.0 * a);

	if((t1 < ERR_MARGIN && t2 < ERR_MARGIN) || (t1 > 1.0 && t2 > 1.0)) return 0;

	if(sp) {
		if(t1 < ERR_MARGIN) t1 = t2;
		if(t2 < ERR_MARGIN) t2 = t1;
		sp->dist = t1 < t2 ? t1 : t2;

		sp->pos.x = ray.orig.x + ray.dir.x * sp->dist;
		sp->pos.y = ray.orig.y + ray.dir.y * sp->dist;
		sp->pos.z = ray.orig.z + ray.dir.z * sp->dist;

		sp->normal.x = (sp->pos.x - sph->pos.x) / sph->rad;
		sp->normal.y = (sp->pos.y - sph->pos.y) / sph->rad;
		sp->normal.z = (sp->pos.z - sph->pos.z) / sph->rad;

		sp->vref = reflect(ray.dir, sp->normal);
		NORMALIZE(sp->vref);
	}
	return 1;
}

/* Load the scene from an extremely simple scene description file */
#define DELIM	" \t\n"
static void load_scene() {
	//copy of the original scene file
	static const char * scene_lines[] = {
		"s -1.5 -0.3 -1 0.7 1.0 0.2  0.05 50.0 0.3",
		"s  1.5 -0.4  0 0.6 0.1 0.85 1.0  50.0 0.4",
		"s	0  -1000  2	999	0.1 0.2  0.6  80.0 0.8",
		"s	0      0  2   1 1.0 0.5  0.1  60.0 0.7",
		"l	-50 100 -50",
		"l	40 40 150",
		"c	0 6 -17		45		0 -1 0"
	};


	char line[256], *ptr, type;

	obj_list = malloc(sizeof(struct sphere));
	obj_list->next = 0;

	int l;
	for(l = 0 ; l < sizeof(scene_lines)/sizeof(char*); l++){
		strcpy(line, scene_lines[l]);

		int i;
		struct vec3 pos, col;
		double rad, spow, refl;

		ptr = line;

		while(*ptr == ' ' || *ptr == '\t') ptr++;
		if(*ptr == '#' || *ptr == '\n') continue;

		if(!(ptr = strtok(line, DELIM))) continue;
		type = *ptr;

		for(i=0; i<3; i++) {
			if(!(ptr = strtok(0, DELIM))) break;
			*((double*)&pos.x + i) = atof(ptr);
		}

		if(type == 'l') {
			lights[lnum++] = pos;
			continue;
		}

		if(!(ptr = strtok(0, DELIM))) continue;
		rad = atof(ptr);

		for(i=0; i<3; i++) {
			if(!(ptr = strtok(0, DELIM))) break;
			*((double*)&col.x + i) = atof(ptr);
		}

		if(type == 'c') {
			cam.pos = pos;
			cam.targ = col;
			cam.fov = rad;
			continue;
		}

		if(!(ptr = strtok(0, DELIM))) continue;
		spow = atof(ptr);

		if(!(ptr = strtok(0, DELIM))) continue;
		refl = atof(ptr);

		if(type == 's') {
			struct sphere *sph = malloc(sizeof *sph);
			sph->next = obj_list->next;
			obj_list->next = sph;

			sph->pos = pos;
			sph->rad = rad;
			sph->mat.col = col;
			sph->mat.spow = spow;
			sph->mat.refl = refl;
		} else {
			fprintf(stderr, "unknown type: %c\n", type);
		}
	}
}


/* provide a millisecond-resolution timer for each system */
#if defined(unix) || defined(__unix__)
#include <time.h>
#include <sys/time.h>
static unsigned long get_msec(void) {
	static struct timeval timeval, first_timeval;

	gettimeofday(&timeval, 0);
	if(first_timeval.tv_sec == 0) {
		first_timeval = timeval;
		return 0;
	}
	return (timeval.tv_sec - first_timeval.tv_sec) * 1000 + (timeval.tv_usec - first_timeval.tv_usec) / 1000;
}
#elif defined(__WIN32__) || defined(WIN32)
#include <windows.h>
static unsigned long get_msec(void) {
	return GetTickCount();
}
#else
#error "I don't know how to measure time on your platform"
#endif

static void *thread_func(void *tdata) {
	int i;
	struct thread_data *td = (struct thread_data*)tdata;

	pthread_mutex_lock(&start_mutex);
	while(!start) {
		pthread_cond_wait(&start_cond, &start_mutex);
	}
	pthread_mutex_unlock(&start_mutex);

	for(i=0; i<td->sl_count; i++) {
		render_scanline(xres, yres, i + td->sl_start, td->pixels, rays_per_pixel);
	}

	return 0;
}

//sequential version
/* render a frame of xsz/ysz dimensions into the provided framebuffer */
static void render(int xsz, int ysz, uint32_t *fb, int samples) {
	int i, j, s;
	double rcp_samples = 1.0 / (double)samples;

	/* for each subpixel, trace a ray through the scene, accumulate the
	 * colors of the subpixels of each pixel, then pack the color and
	 * put it into the framebuffer.
	 * XXX: assumes contiguous scanlines with NO padding, and 32bit pixels.
	 */
	for(j=0; j<ysz; j++) {
		for(i=0; i<xsz; i++) {
			double r, g, b;
			r = g = b = 0.0;

			for(s=0; s<samples; s++) {
				struct vec3 col = trace(get_primary_ray(i, j, s), 0);
				r += col.x;
				g += col.y;
				b += col.z;
			}

			r = r * rcp_samples;
			g = g * rcp_samples;
			b = b * rcp_samples;

			*fb++ = ((uint32_t)(MIN(r, 1.0) * 255.0) & 0xff) << RSHIFT |
					((uint32_t)(MIN(g, 1.0) * 255.0) & 0xff) << GSHIFT |
					((uint32_t)(MIN(b, 1.0) * 255.0) & 0xff) << BSHIFT;
		}
	}
}

int cray_f(const int _xres, const int _yres, const int _rays_per_pixel) {
	int i;
	unsigned long rend_time, start_time;
	uint32_t *pixels;

	xres = _xres;
	yres = _yres;
	rays_per_pixel = _rays_per_pixel;

	FILE *outfile = fopen(FILE_OUT, "wb");
	if(outfile == NULL){
		perror("fopen");
		return 0;
	}

	if(!(pixels = malloc(xres * yres * sizeof *pixels))) {
		perror("pixel buffer allocation failed");
		return EXIT_FAILURE;
	}
	load_scene();

	/* initialize the random number tables for the jitter */
	for(i=0; i<NRAN; i++) urand[i].x = (double)rand() / RAND_MAX - 0.5;
	for(i=0; i<NRAN; i++) urand[i].y = (double)rand() / RAND_MAX - 0.5;
	for(i=0; i<NRAN; i++) irand[i] = (int)(NRAN * ((double)rand() / RAND_MAX));

	start_time = get_msec();
	render(xres, yres, pixels, rays_per_pixel);
	rend_time = get_msec() - start_time;

	/* output statistics to stderr */
	//fprintf(stderr, "Rendering took: %lu seconds (%lu milliseconds)\n", rend_time / 1000, rend_time);

	/* output the image */
	fprintf(outfile, "P6\n%d %d\n255\n", xres, yres);
	for(i=0; i<xres * yres; i++) {
		fputc((pixels[i] >> RSHIFT) & 0xff, outfile);
		fputc((pixels[i] >> GSHIFT) & 0xff, outfile);
		fputc((pixels[i] >> BSHIFT) & 0xff, outfile);
	}
	fflush(outfile);
	if(outfile != stdout) fclose(outfile);

	free(pixels);
	struct sphere *iter = obj_list->next;
	while(iter){
		struct sphere *s = iter;
		iter = iter->next;
		free(s);
	}
	free(obj_list);

	return rend_time;
};

int cray_mt(int thread_num, const int _xres, const int _yres, const int _rays_per_pixel) {
	int i;
	unsigned long rend_time, start_time;
	uint32_t *pixels;
	double sl, sl_per_thread;

	xres = _xres;
	yres = _yres;
	rays_per_pixel = _rays_per_pixel;

	FILE *outfile = fopen(FILE_OUT, "wb");
	if(outfile == NULL){
		perror("fopen");
		return 0;
	}

	if(!(pixels = malloc(xres * yres * sizeof *pixels))) {
		perror("pixel buffer allocation failed");
		return EXIT_FAILURE;
	}
	load_scene();

	/* initialize the random number tables for the jitter */
	for(i=0; i<NRAN; i++) urand[i].x = (double)rand() / RAND_MAX - 0.5;
	for(i=0; i<NRAN; i++) urand[i].y = (double)rand() / RAND_MAX - 0.5;
	for(i=0; i<NRAN; i++) irand[i] = (int)(NRAN * ((double)rand() / RAND_MAX));

	if(thread_num > yres) {
		fprintf(stderr, "more threads than scanlines specified, reducing number of threads to %d\n", yres);
		thread_num = yres;
	}

	if(!(threads = malloc(thread_num * sizeof *threads))) {
		perror("failed to allocate thread table");
		return EXIT_FAILURE;
	}

	sl = 0.0;
	sl_per_thread = (double)yres / (double)thread_num;
	for(i=0; i<thread_num; i++) {
		threads[i].sl_start = (int)sl;
		sl += sl_per_thread;
		threads[i].sl_count = (int)sl - threads[i].sl_start;
		threads[i].pixels = pixels;

		if(pthread_create(&threads[i].tid, 0, thread_func, &threads[i]) != 0) {
			perror("failed to spawn thread");
			return EXIT_FAILURE;
		}
	}
	threads[thread_num - 1].sl_count = yres - threads[thread_num - 1].sl_start;

	//fprintf(stderr, VER_STR, VER_MAJOR, VER_MINOR);

	pthread_mutex_lock(&start_mutex);
	start_time = get_msec();
	start = 1;
	pthread_cond_broadcast(&start_cond);
	pthread_mutex_unlock(&start_mutex);

	for(i=0; i<thread_num; i++) {
		pthread_join(threads[i].tid, 0);
	}
	rend_time = get_msec() - start_time;

	/* output statistics to stderr */
	//fprintf(stderr, "Rendering took: %lu seconds (%lu milliseconds)\n", rend_time / 1000, rend_time);

	/* output the image */
	fprintf(outfile, "P6\n%d %d\n255\n", xres, yres);
	for(i=0; i<xres * yres; i++) {
		fputc((pixels[i] >> RSHIFT) & 0xff, outfile);
		fputc((pixels[i] >> GSHIFT) & 0xff, outfile);
		fputc((pixels[i] >> BSHIFT) & 0xff, outfile);
	}
	fflush(outfile);
	if(outfile != stdout) fclose(outfile);

	free(pixels);
	free(threads);
	struct sphere *iter = obj_list->next;
	while(iter){
		struct sphere *s = iter;
		iter = iter->next;
		free(s);
	}
	free(obj_list);

	return rend_time;
};