/*
 * Unittest for named (struct/array) initializer c99-to-c89 conversion.
 */

typedef struct PixFmtInfo {
    int nb_channels, color_type, pixel_type, is_alpha, depth;
} PixFmtInfo;

enum PixelFormat {
    PIX_FMT_YUV420P,
    PIX_FMT_YUYV422,
    PIX_FMT_YUVJ420P,
    PIX_FMT_RGB24,
    PIX_FMT_RGBA,
    PIX_FMT_DUMMY,
    PIX_FMT_GRAY8,
};

enum ColorType {
    COLOR_RGB,
    COLOR_GRAY,
    COLOR_YUV,
    COLOR_YUV_JPEG,
};

enum PixelType {
    PIXEL_PLANAR = 2,
    PIXEL_PACKED = PIXEL_PLANAR + 1,
    PIXEL_PALETTE = 5,
};

static const struct PixFmtInfo pix_fmt_info[] = {
    [PIX_FMT_RGBA] = {
        .nb_channels = 4,
        .color_type  = COLOR_RGB,
        .pixel_type  = PIXEL_PACKED,
        .is_alpha    = 1,
        .depth       = 32,
    }, [PIX_FMT_RGB24] = {
        .nb_channels = 3,
        .color_type  = COLOR_RGB,
        .pixel_type  = PIXEL_PACKED,
        .depth       = 24,
    }, [PIX_FMT_YUVJ420P] = {
        .nb_channels = 3,
        .color_type  = COLOR_YUV_JPEG,
        .pixel_type  = PIXEL_PLANAR,
        .depth       = 12,
    }, [PIX_FMT_YUV420P] = {
        .nb_channels = 3,
        .color_type  = COLOR_YUV,
        .pixel_type  = PIXEL_PLANAR,
        .depth       = 12,
    }, [PIX_FMT_GRAY8] = {
        .nb_channels = 1,
        .color_type  = COLOR_GRAY,
        .depth       = 8,
    }, [PIX_FMT_YUYV422] = {
        .nb_channels = 3,
        .color_type  = COLOR_YUV,
        .pixel_type  = PIXEL_PACKED,
        .depth       = 16,
    },
};

static const struct {
    int a;
    struct { int c, d; } b;
} random_values2 = {
    .b = { .d = 1, },
}, random_values3 = {
    0, { .d = 1, },
};

static const struct {
    int a, b;
} random_values[] = {
    { .b = 1, },
    [3] = { .b = 3 },
};

static const struct PixFmtInfo info2 = {
    3, COLOR_YUV_JPEG, PIX_FMT_YUVJ420P, .depth = 12
};

typedef struct {
    const char *name;
    union {
        void *dst_ptr;
        int (*func_arg)(void);
        double dbl;
    } u;
} OptionDef;

static int do_nothing(void) { }
static const OptionDef options[] = {
  { "name", {(void*)0,},},
  { "name2", {.func_arg=do_nothing,},},
  { "name3", {.dbl = (1.0/3 + 2/3)/2,},},
};

union av_intfloat32 {
    int   i;
    float f;
};

int foo(float f) {
    union av_intfloat32 s = { .f = f };
    int other;
    return s.i;
}

int main(int argc, char *argv[])
{
    return pix_fmt_info[argc].depth;
}
