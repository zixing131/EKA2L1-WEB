#include <kernel/kernel.h>
#include <kernel/libmanager.h>
#include <kernel/process.h>

#include <common/cvt.h>
#include <common/fileutils.h>
#include <common/log.h>
#include <vfs/vfs.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#include <stb_image.h>

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

namespace eka2l1::hle {
    namespace {
        constexpr int k_lcd_w = 240;
        constexpr int k_lcd_h = 320;
        constexpr int k_max_steps = 80000000;
        constexpr int k_max_depth = 96;
        constexpr int k_font_px = 12;

        enum : std::uint8_t {
            k_utf8 = 1,
            k_int = 3,
            k_float = 4,
            k_long = 5,
            k_double = 6,
            k_class = 7,
            k_string = 8,
            k_field = 9,
            k_method = 10,
            k_imethod = 11,
            k_nat = 12
        };

        enum : int {
            k_obj_java = 0,
            k_obj_string = 1,
            k_obj_array = 2,
            k_obj_image = 3,
            k_obj_graphics = 4,
            k_obj_display = 5,
            k_obj_sb = 6,
            k_obj_stream = 7,
            k_obj_class = 8,
            k_obj_vector = 9,
            k_obj_random = 10,
            k_obj_rms = 11,
            k_obj_thread = 12
        };

        struct cf_cp {
            std::uint8_t tag = 0;
            std::uint16_t a = 0;
            std::uint16_t b = 0;
            std::int32_t i = 0;
            std::int64_t l = 0;
            std::string utf;
        };

        struct cf_field {
            std::uint16_t flags = 0;
            std::uint16_t name = 0;
            std::uint16_t desc = 0;
        };

        struct cf_method {
            std::uint16_t flags = 0;
            std::uint16_t name = 0;
            std::uint16_t desc = 0;
            std::uint16_t max_stack = 8;
            std::uint16_t max_locals = 8;
            std::vector<std::uint8_t> code;
        };

        struct cf_class;

        struct host_obj {
            int kind = k_obj_java;
            cf_class *clazz = nullptr;
            std::string str;
            std::unordered_map<std::string, std::int32_t> fields;
            std::vector<std::int32_t> arr;
            int arr_is_ref = 0;
            std::vector<std::uint32_t> pixels;
            int w = 0;
            int h = 0;
            int color = 0xFF000000;
            int tx = 0;
            int ty = 0;
            int cx = 0;
            int cy = 0;
            int cw = k_lcd_w;
            int ch = k_lcd_h;
            int target = 0;
            std::vector<std::uint8_t> bytes;
            int pos = 0;
            int nested = 0;
            int seed = 1;
        };

        struct cf_class {
            std::string name;
            std::string super;
            std::vector<cf_cp> cp;
            std::vector<cf_field> fields;
            std::vector<cf_method> methods;
            std::unordered_map<std::string, std::int32_t> statics;
        };

        static std::unordered_map<std::string, std::unique_ptr<cf_class>> g_classes;
        static std::vector<std::unique_ptr<host_obj>> g_objs;
        static std::unordered_map<std::string, int> g_intern;
        static int g_display = 0;
        static int g_current = 0;
        static int g_fb = 0;
        static int g_font = 0;
        static bool g_event_ready = false;
        static kernel::process *g_pr = nullptr;

        static std::int32_t run_java(cf_class *c, cf_method *m, int this_ref, const std::int32_t *args, int nargs, int depth);
        static cf_method *find_method(cf_class *c, const std::string &name, const std::string &sig);

        static std::uint16_t be16(const std::uint8_t *p) {
            return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
        }
        static std::uint32_t be32(const std::uint8_t *p) {
            return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16)
                | (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
        }

        static bool guest_read(kernel::process *pr, const std::u16string &path, std::vector<std::uint8_t> &out) {
            if (!pr) {
                return false;
            }
            kernel_system *kern = pr->get_kernel_object_owner();
            io_system *io = kern ? kern->get_io_system() : nullptr;
            if (!io) {
                return false;
            }
            auto f = io->open_file(path, READ_MODE | BIN_MODE);
            if (!f) {
                return false;
            }
            const auto sz = f->size();
            if (sz > 8u * 1024u * 1024u) {
                f->close();
                return false;
            }
            out.resize(static_cast<std::size_t>(sz));
            const auto n = f->read_file(out.data(), 1, static_cast<std::uint32_t>(out.size()));
            f->close();
            return n == out.size();
        }

        static bool guest_read_any(kernel::process *pr, const std::string &rel, std::vector<std::uint8_t> &out) {
            std::string norm = rel;
            for (char &c : norm) {
                if (c == '/') {
                    c = '\\';
                }
            }
            while (!norm.empty() && (norm[0] == '\\')) {
                norm.erase(norm.begin());
            }
            const std::u16string urel = common::utf8_to_ucs2(norm);
            const std::u16string homes[] = {
                u"C:\\private\\102033E6\\0\\",
                u"C:\\Private\\102033E6\\0\\",
                u"C:\\private\\102033e6\\0\\",
                u"C:\\Private\\102033E6\\536870916\\",
                u"C:\\Private\\102033E6\\20000004\\",
            };
            for (const auto &home : homes) {
                if (guest_read(pr, home + urel, out)) {
                    return true;
                }
            }
            return guest_read(pr, u"C:\\" + urel, out);
        }

        static const char *utf(const cf_class *c, unsigned i) {
            if (!c || (i == 0) || (i >= c->cp.size()) || (c->cp[i].tag != k_utf8)) {
                return "";
            }
            return c->cp[i].utf.c_str();
        }

        static std::string cp_class_name(const cf_class *c, unsigned i) {
            if (!c || (i == 0) || (i >= c->cp.size()) || (c->cp[i].tag != k_class)) {
                return {};
            }
            return utf(c, c->cp[i].a);
        }

        static std::string cp_string(const cf_class *c, unsigned i) {
            if (!c || (i == 0) || (i >= c->cp.size()) || (c->cp[i].tag != k_string)) {
                return {};
            }
            return utf(c, c->cp[i].a);
        }

        static void cp_method(const cf_class *c, unsigned i, std::string &cls, std::string &name, std::string &sig) {
            cls.clear();
            name.clear();
            sig.clear();
            if (!c || (i == 0) || (i >= c->cp.size())) {
                return;
            }
            const auto &e = c->cp[i];
            if ((e.tag != k_method) && (e.tag != k_imethod) && (e.tag != k_field)) {
                return;
            }
            cls = cp_class_name(c, e.a);
            if ((e.b == 0) || (e.b >= c->cp.size()) || (c->cp[e.b].tag != k_nat)) {
                return;
            }
            name = utf(c, c->cp[e.b].a);
            sig = utf(c, c->cp[e.b].b);
        }

        static bool parse_class(const std::vector<std::uint8_t> &buf, cf_class &out) {
            if (buf.size() < 16) {
                return false;
            }
            if (be32(buf.data()) != 0xCAFEBABEu) {
                return false;
            }
            const unsigned cpc = be16(buf.data() + 8);
            if ((cpc < 2) || (cpc > 8000)) {
                return false;
            }
            out.cp.assign(cpc, {});
            std::size_t p = 10;
            auto need = [&](std::size_t n) { return p + n <= buf.size(); };
            for (unsigned i = 1; i < cpc; ++i) {
                if (!need(1)) {
                    return false;
                }
                const std::uint8_t tag = buf[p++];
                out.cp[i].tag = tag;
                if (tag == k_utf8) {
                    if (!need(2)) {
                        return false;
                    }
                    const unsigned n = be16(buf.data() + p);
                    p += 2;
                    if (!need(n)) {
                        return false;
                    }
                    out.cp[i].utf.assign(reinterpret_cast<const char *>(buf.data() + p), n);
                    p += n;
                } else if ((tag == k_class) || (tag == k_string)) {
                    if (!need(2)) {
                        return false;
                    }
                    out.cp[i].a = be16(buf.data() + p);
                    p += 2;
                } else if ((tag == k_int) || (tag == k_float)) {
                    if (!need(4)) {
                        return false;
                    }
                    out.cp[i].i = static_cast<std::int32_t>(be32(buf.data() + p));
                    p += 4;
                } else if ((tag == k_long) || (tag == k_double)) {
                    if (!need(8)) {
                        return false;
                    }
                    const std::uint64_t hi = be32(buf.data() + p);
                    const std::uint64_t lo = be32(buf.data() + p + 4);
                    out.cp[i].l = static_cast<std::int64_t>((hi << 32) | lo);
                    p += 8;
                    ++i;
                } else if ((tag == k_field) || (tag == k_method) || (tag == k_imethod) || (tag == k_nat)) {
                    if (!need(4)) {
                        return false;
                    }
                    out.cp[i].a = be16(buf.data() + p);
                    out.cp[i].b = be16(buf.data() + p + 2);
                    p += 4;
                } else {
                    return false;
                }
            }
            if (!need(8)) {
                return false;
            }
            p += 2;
            const unsigned this_c = be16(buf.data() + p);
            p += 2;
            const unsigned super_c = be16(buf.data() + p);
            p += 2;
            out.name = cp_class_name(&out, this_c);
            out.super = cp_class_name(&out, super_c);
            const unsigned nif = be16(buf.data() + p);
            p += 2;
            p += static_cast<std::size_t>(nif) * 2u;
            if (!need(2)) {
                return false;
            }
            const unsigned nf = be16(buf.data() + p);
            p += 2;
            out.fields.resize(nf);
            for (unsigned i = 0; i < nf; ++i) {
                if (!need(8)) {
                    return false;
                }
                out.fields[i].flags = be16(buf.data() + p);
                out.fields[i].name = be16(buf.data() + p + 2);
                out.fields[i].desc = be16(buf.data() + p + 4);
                const unsigned na = be16(buf.data() + p + 6);
                p += 8;
                for (unsigned a = 0; a < na; ++a) {
                    if (!need(6)) {
                        return false;
                    }
                    const unsigned alen = be32(buf.data() + p + 2);
                    p += 6 + alen;
                }
            }
            if (!need(2)) {
                return false;
            }
            const unsigned nm = be16(buf.data() + p);
            p += 2;
            out.methods.resize(nm);
            for (unsigned i = 0; i < nm; ++i) {
                if (!need(8)) {
                    return false;
                }
                out.methods[i].flags = be16(buf.data() + p);
                out.methods[i].name = be16(buf.data() + p + 2);
                out.methods[i].desc = be16(buf.data() + p + 4);
                const unsigned na = be16(buf.data() + p + 6);
                p += 8;
                for (unsigned a = 0; a < na; ++a) {
                    if (!need(6)) {
                        return false;
                    }
                    const unsigned nidx = be16(buf.data() + p);
                    const unsigned alen = be32(buf.data() + p + 2);
                    p += 6;
                    if (!need(alen)) {
                        return false;
                    }
                    if (std::strcmp(utf(&out, nidx), "Code") == 0) {
                        if (alen >= 8) {
                            out.methods[i].max_stack = be16(buf.data() + p);
                            out.methods[i].max_locals = be16(buf.data() + p + 2);
                            const unsigned clen = be32(buf.data() + p + 4);
                            if ((8u + clen) <= alen) {
                                out.methods[i].code.assign(buf.data() + p + 8, buf.data() + p + 8 + clen);
                            }
                        }
                    }
                    p += alen;
                }
            }
            return !out.name.empty();
        }

        static cf_class *load_class(kernel::process *pr, const std::string &name) {
            auto it = g_classes.find(name);
            if (it != g_classes.end()) {
                return it->second.get();
            }
            if (name.rfind("java/", 0) == 0 || name.rfind("javax/", 0) == 0
                || name.rfind("com/sun/", 0) == 0) {
                return nullptr;
            }
            std::vector<std::uint8_t> buf;
            std::string path = name + ".class";
            if (!guest_read_any(pr, path, buf)) {
                LOG_WARN(EMULATED_STDOUT, "[j9-nf] host-class-miss '{}'", name);
                return nullptr;
            }
            auto c = std::make_unique<cf_class>();
            if (!parse_class(buf, *c) || (c->name != name && c->name.find(name) == std::string::npos)) {
                LOG_WARN(EMULATED_STDOUT, "[j9-nf] host-class-bad '{}' parsed='{}' bytes={}",
                    name, c->name, buf.size());
                return nullptr;
            }
            cf_class *raw = c.get();
            g_classes.emplace(name, std::move(c));
            LOG_WARN(EMULATED_STDOUT, "[j9-nf] host-class '{}' super='{}' methods={} fields={}",
                raw->name, raw->super, raw->methods.size(), raw->fields.size());
            if (cf_method *clinit = find_method(raw, "<clinit>", "()V")) {
                LOG_WARN(EMULATED_STDOUT, "[j9-nf] host-clinit {}", raw->name);
                run_java(raw, clinit, 0, nullptr, 0, 0);
            }
            return raw;
        }

        static int alloc_obj(int kind) {
            auto o = std::make_unique<host_obj>();
            o->kind = kind;
            g_objs.push_back(std::move(o));
            return static_cast<int>(g_objs.size());
        }

        static host_obj *obj(int ref) {
            if ((ref <= 0) || (static_cast<unsigned>(ref) > g_objs.size())) {
                return nullptr;
            }
            return g_objs[static_cast<unsigned>(ref) - 1].get();
        }

        static int intern(const std::string &s) {
            auto it = g_intern.find(s);
            if (it != g_intern.end()) {
                return it->second;
            }
            const int r = alloc_obj(k_obj_string);
            obj(r)->str = s;
            g_intern.emplace(s, r);
            return r;
        }

        static std::string as_str(int ref) {
            host_obj *o = obj(ref);
            return (o && (o->kind == k_obj_string)) ? o->str : std::string();
        }

        static std::uint32_t pack_rgba(int rgb, int alpha = 255) {
            const unsigned r = static_cast<unsigned>((rgb >> 16) & 255);
            const unsigned g = static_cast<unsigned>((rgb >> 8) & 255);
            const unsigned b = static_cast<unsigned>(rgb & 255);
            const unsigned a = static_cast<unsigned>(alpha) & 255u;
            return r | (g << 8) | (b << 16) | (a << 24);
        }

        static int ensure_fb() {
            if (g_fb && obj(g_fb)) {
                return g_fb;
            }
            g_fb = alloc_obj(k_obj_image);
            host_obj *o = obj(g_fb);
            o->w = k_lcd_w;
            o->h = k_lcd_h;
            o->pixels.assign(static_cast<unsigned>(k_lcd_w * k_lcd_h), pack_rgba(0x000000));
            return g_fb;
        }

        static host_obj *fb() {
            return obj(ensure_fb());
        }

        static void present() {
            host_obj *o = fb();
            if (!o || o->pixels.empty()) {
                return;
            }
            // Host pixels are RGBA (stb / Java RGB). Window-server bitmaps are
            // guest BGRA; WASM update_bitmap then swaps R/B for WebGL. Sending
            // RGBA here made skin/wood land in the blue channel.
            static std::vector<std::uint32_t> upload;
            upload.resize(o->pixels.size());
            for (std::size_t i = 0; i < o->pixels.size(); ++i) {
                const std::uint32_t p = o->pixels[i];
                upload[i] = (p & 0xFF00FF00u) | ((p & 0xFFu) << 16) | ((p >> 16) & 0xFFu);
            }
            const bool ok = j9_present_surface(upload.data(), o->w, o->h);
            static int n = 0;
            if (n < 4) {
                ++n;
                LOG_WARN(EMULATED_STDOUT, "[j9-nf] host-present {}x{} ok={}", o->w, o->h, ok ? 1 : 0);
            }
        }

        static void blit(host_obj *dst, int dx, int dy, host_obj *src, int sx, int sy, int sw, int sh) {
            if (!dst || !src || dst->pixels.empty() || src->pixels.empty()) {
                return;
            }
            if (sw <= 0) {
                sw = src->w;
            }
            if (sh <= 0) {
                sh = src->h;
            }
            for (int y = 0; y < sh; ++y) {
                const int dy2 = dy + y;
                const int sy2 = sy + y;
                if ((dy2 < 0) || (dy2 >= dst->h) || (sy2 < 0) || (sy2 >= src->h)) {
                    continue;
                }
                for (int x = 0; x < sw; ++x) {
                    const int dx2 = dx + x;
                    const int sx2 = sx + x;
                    if ((dx2 < 0) || (dx2 >= dst->w) || (sx2 < 0) || (sx2 >= src->w)) {
                        continue;
                    }
                    const std::uint32_t px = src->pixels[static_cast<unsigned>(sy2 * src->w + sx2)];
                    if (((px >> 24) & 255u) == 0) {
                        continue;
                    }
                    dst->pixels[static_cast<unsigned>(dy2 * dst->w + dx2)] = px;
                }
            }
        }

        static void fill_rect(host_obj *g, int x, int y, int w, int h) {
            host_obj *dst = (g && g->target) ? obj(g->target) : fb();
            if (!dst) {
                return;
            }
            x += g ? g->tx : 0;
            y += g ? g->ty : 0;
            const int x0 = std::max(x, g ? g->cx : 0);
            const int y0 = std::max(y, g ? g->cy : 0);
            const int x1 = std::min(x + w, g ? (g->cx + g->cw) : dst->w);
            const int y1 = std::min(y + h, g ? (g->cy + g->ch) : dst->h);
            const std::uint32_t c = g ? static_cast<std::uint32_t>(g->color) : pack_rgba(0);
            for (int yy = y0; yy < y1; ++yy) {
                if ((yy < 0) || (yy >= dst->h)) {
                    continue;
                }
                for (int xx = x0; xx < x1; ++xx) {
                    if ((xx < 0) || (xx >= dst->w)) {
                        continue;
                    }
                    dst->pixels[static_cast<unsigned>(yy * dst->w + xx)] = c;
                }
            }
        }

        static std::uint32_t utf8_next(const std::string &s, std::size_t &i) {
            if (i >= s.size()) {
                return 0;
            }
            const unsigned char c = static_cast<unsigned char>(s[i]);
            auto need = [&](unsigned n) {
                return (i + n) <= s.size();
            };
            auto cont = [&](unsigned off) {
                return static_cast<unsigned char>(s[i + off]) & 0x3Fu;
            };
            if (c < 0x80) {
                ++i;
                return c;
            }
            if (((c & 0xE0) == 0xC0) && need(2) && ((static_cast<unsigned char>(s[i + 1]) & 0xC0) == 0x80)) {
                const std::uint32_t cp = ((c & 0x1Fu) << 6) | cont(1);
                i += 2;
                return cp;
            }
            if (((c & 0xF0) == 0xE0) && need(3)
                && ((static_cast<unsigned char>(s[i + 1]) & 0xC0) == 0x80)
                && ((static_cast<unsigned char>(s[i + 2]) & 0xC0) == 0x80)) {
                const std::uint32_t cp = ((c & 0x0Fu) << 12) | (cont(1) << 6) | cont(2);
                i += 3;
                return cp;
            }
            if (((c & 0xF8) == 0xF0) && need(4)
                && ((static_cast<unsigned char>(s[i + 1]) & 0xC0) == 0x80)
                && ((static_cast<unsigned char>(s[i + 2]) & 0xC0) == 0x80)
                && ((static_cast<unsigned char>(s[i + 3]) & 0xC0) == 0x80)) {
                const std::uint32_t cp = ((c & 0x07u) << 18) | (cont(1) << 12) | (cont(2) << 6) | cont(3);
                i += 4;
                return cp;
            }
            ++i;
            return c;
        }

        static int utf8_len(const std::string &s) {
            int n = 0;
            for (std::size_t i = 0; i < s.size();) {
                utf8_next(s, i);
                ++n;
            }
            return n;
        }

        static std::uint32_t utf8_at(const std::string &s, int index) {
            if (index < 0) {
                return 0;
            }
            int n = 0;
            for (std::size_t i = 0; i < s.size();) {
                const std::uint32_t cp = utf8_next(s, i);
                if (n == index) {
                    return cp;
                }
                ++n;
            }
            return 0;
        }

        static std::string utf8_substr(const std::string &s, int begin, int end) {
            if (begin < 0) {
                begin = 0;
            }
            if (end < begin) {
                end = begin;
            }
            std::string out;
            int n = 0;
            for (std::size_t i = 0; i < s.size();) {
                const std::size_t mark = i;
                utf8_next(s, i);
                if ((n >= begin) && (n < end)) {
                    out.append(s, mark, i - mark);
                }
                ++n;
                if (n >= end) {
                    break;
                }
            }
            return out;
        }

        struct host_glyph {
            std::vector<std::uint8_t> bits;
            int w = 0;
            int h = 0;
            int xoff = 0;
            int yoff = 0;
            int adv = 0;
        };

        static std::vector<std::uint8_t> g_ttf;
        static stbtt_fontinfo g_tt{};
        static bool g_tt_ok = false;
        static float g_tt_scale = 1.f;
        static int g_tt_ascent = k_font_px;
        static std::unordered_map<std::uint32_t, host_glyph> g_glyphs;

        static bool ensure_host_font() {
            if (g_tt_ok) {
                return true;
            }
            if (g_ttf.empty()) {
                static const char *k_paths[] = {
                    ".//fonts//DroidSansFallback.ttf",
                    "fonts/DroidSansFallback.ttf",
                    "./fonts/DroidSansFallback.ttf",
                };
                for (const char *path : k_paths) {
                    FILE *f = common::open_c_file(path, "rb");
                    if (!f) {
                        continue;
                    }
                    std::fseek(f, 0, SEEK_END);
                    const long sz = std::ftell(f);
                    std::fseek(f, 0, SEEK_SET);
                    if (sz > 0) {
                        g_ttf.resize(static_cast<std::size_t>(sz));
                        if (std::fread(g_ttf.data(), 1, g_ttf.size(), f) != g_ttf.size()) {
                            g_ttf.clear();
                        }
                    }
                    std::fclose(f);
                    if (!g_ttf.empty()) {
                        LOG_WARN(EMULATED_STDOUT, "[j9-nf] host-font '{}'", path);
                        break;
                    }
                }
            }
            if (g_ttf.empty() || !stbtt_InitFont(&g_tt, g_ttf.data(), 0)) {
                LOG_WARN(EMULATED_STDOUT, "[j9-nf] host-font-miss DroidSansFallback.ttf");
                return false;
            }
            g_tt_scale = stbtt_ScaleForPixelHeight(&g_tt, static_cast<float>(k_font_px));
            int ascent = 0, descent = 0, gap = 0;
            stbtt_GetFontVMetrics(&g_tt, &ascent, &descent, &gap);
            g_tt_ascent = static_cast<int>(ascent * g_tt_scale);
            g_tt_ok = true;
            return true;
        }

        static const host_glyph &glyph_of(std::uint32_t cp) {
            static host_glyph empty;
            if (!ensure_host_font()) {
                return empty;
            }
            auto it = g_glyphs.find(cp);
            if (it != g_glyphs.end()) {
                return it->second;
            }
            host_glyph gl;
            int adv = 0, lsb = 0;
            stbtt_GetCodepointHMetrics(&g_tt, static_cast<int>(cp), &adv, &lsb);
            gl.adv = std::max(1, static_cast<int>(adv * g_tt_scale));
            unsigned char *bmp = stbtt_GetCodepointBitmap(&g_tt, g_tt_scale, g_tt_scale,
                static_cast<int>(cp), &gl.w, &gl.h, &gl.xoff, &gl.yoff);
            if (bmp && (gl.w > 0) && (gl.h > 0)) {
                gl.bits.assign(bmp, bmp + (gl.w * gl.h));
                stbtt_FreeBitmap(bmp, nullptr);
            }
            auto ins = g_glyphs.emplace(cp, std::move(gl));
            return ins.first->second;
        }

        static int measure_text(const std::string &s) {
            int w = 0;
            for (std::size_t i = 0; i < s.size();) {
                const std::uint32_t cp = utf8_next(s, i);
                if ((cp == '\n') || (cp == '\r')) {
                    continue;
                }
                const int adv = glyph_of(cp).adv;
                w += adv ? adv : ((cp < 0x80) ? (k_font_px / 2) : k_font_px);
            }
            return w;
        }

        static void blit_glyph(host_obj *g, int dx, int dy, const host_glyph &gl) {
            if (gl.bits.empty()) {
                return;
            }
            host_obj *dst = (g && g->target) ? obj(g->target) : fb();
            if (!dst || dst->pixels.empty()) {
                return;
            }
            dx += g ? g->tx : 0;
            dy += g ? g->ty : 0;
            const int cx0 = g ? g->cx : 0;
            const int cy0 = g ? g->cy : 0;
            const int cx1 = g ? (g->cx + g->cw) : dst->w;
            const int cy1 = g ? (g->cy + g->ch) : dst->h;
            const std::uint32_t col = g ? static_cast<std::uint32_t>(g->color) : pack_rgba(0xFFFFFF);
            const unsigned sr = col & 255u;
            const unsigned sg = (col >> 8) & 255u;
            const unsigned sb = (col >> 16) & 255u;
            for (int y = 0; y < gl.h; ++y) {
                const int py = dy + y;
                if ((py < cy0) || (py >= cy1) || (py < 0) || (py >= dst->h)) {
                    continue;
                }
                for (int x = 0; x < gl.w; ++x) {
                    const unsigned cov = gl.bits[static_cast<unsigned>(y * gl.w + x)];
                    if (!cov) {
                        continue;
                    }
                    const int px = dx + x;
                    if ((px < cx0) || (px >= cx1) || (px < 0) || (px >= dst->w)) {
                        continue;
                    }
                    std::uint32_t &p = dst->pixels[static_cast<unsigned>(py * dst->w + px)];
                    const unsigned dr = p & 255u;
                    const unsigned dg = (p >> 8) & 255u;
                    const unsigned db = (p >> 16) & 255u;
                    const unsigned nr = (sr * cov + dr * (255u - cov)) / 255u;
                    const unsigned ng = (sg * cov + dg * (255u - cov)) / 255u;
                    const unsigned nb = (sb * cov + db * (255u - cov)) / 255u;
                    p = nr | (ng << 8) | (nb << 16) | (255u << 24);
                }
            }
        }

        static void draw_string(host_obj *g, const std::string &text, int x, int y, int anc) {
            const int tw = measure_text(text);
            if (anc & 1) {
                x -= tw / 2;
            } else if (anc & 8) {
                x -= tw;
            }
            if (anc & 2) {
                y -= k_font_px / 2;
            } else if (anc & 32) {
                y -= k_font_px;
            }
            int baseline = y + g_tt_ascent;
            if (anc & 64) {
                baseline = y;
            }
            int pen = x;
            for (std::size_t i = 0; i < text.size();) {
                const std::uint32_t cp = utf8_next(text, i);
                if (cp == '\n') {
                    pen = x;
                    baseline += k_font_px;
                    continue;
                }
                if (cp == '\r') {
                    continue;
                }
                const host_glyph &gl = glyph_of(cp);
                if (gl.bits.empty() && (cp > 32)) {
                    fill_rect(g, pen, baseline - g_tt_ascent, std::max(gl.adv, k_font_px) - 1, k_font_px);
                } else {
                    blit_glyph(g, pen + gl.xoff, baseline + gl.yoff, gl);
                }
                pen += gl.adv ? gl.adv : k_font_px;
            }
        }

        static int load_image(const std::string &path) {
            std::string rel = path;
            if (!rel.empty() && rel[0] == '/') {
                rel.erase(rel.begin());
            }
            std::vector<std::uint8_t> buf;
            if (!guest_read_any(g_pr, rel, buf) || buf.empty()) {
                LOG_WARN(EMULATED_STDOUT, "[j9-nf] host-image-miss '{}'", path);
                const int r = alloc_obj(k_obj_image);
                obj(r)->w = 16;
                obj(r)->h = 16;
                obj(r)->pixels.assign(256, pack_rgba(0xFF00FF));
                return r;
            }
            int w = 0, h = 0, n = 0;
            stbi_uc *pix = stbi_load_from_memory(buf.data(), static_cast<int>(buf.size()), &w, &h, &n, 4);
            if (!pix || (w <= 0) || (h <= 0)) {
                LOG_WARN(EMULATED_STDOUT, "[j9-nf] host-image-decode-fail '{}' bytes={}", path, buf.size());
                const int r = alloc_obj(k_obj_image);
                obj(r)->w = 16;
                obj(r)->h = 16;
                obj(r)->pixels.assign(256, pack_rgba(0x00FFFF));
                return r;
            }
            const int r = alloc_obj(k_obj_image);
            host_obj *o = obj(r);
            o->w = w;
            o->h = h;
            o->pixels.resize(static_cast<unsigned>(w * h));
            for (int i = 0; i < w * h; ++i) {
                const unsigned off = static_cast<unsigned>(i) * 4u;
                o->pixels[static_cast<unsigned>(i)] = pix[off] | (static_cast<std::uint32_t>(pix[off + 1]) << 8)
                    | (static_cast<std::uint32_t>(pix[off + 2]) << 16)
                    | (static_cast<std::uint32_t>(pix[off + 3]) << 24);
            }
            stbi_image_free(pix);
            LOG_WARN(EMULATED_STDOUT, "[j9-nf] host-image '{}' {}x{}", path, w, h);
            return r;
        }

        static int image_from_bytes(const std::uint8_t *data, int n) {
            if (!data || (n <= 0)) {
                return 0;
            }
            int w = 0, h = 0, ch = 0;
            stbi_uc *pix = stbi_load_from_memory(data, n, &w, &h, &ch, 4);
            if (!pix || (w <= 0) || (h <= 0)) {
                return 0;
            }
            const int r = alloc_obj(k_obj_image);
            host_obj *o = obj(r);
            o->w = w;
            o->h = h;
            o->pixels.resize(static_cast<unsigned>(w * h));
            for (int i = 0; i < w * h; ++i) {
                const unsigned off = static_cast<unsigned>(i) * 4u;
                o->pixels[static_cast<unsigned>(i)] = pix[off] | (static_cast<std::uint32_t>(pix[off + 1]) << 8)
                    | (static_cast<std::uint32_t>(pix[off + 2]) << 16)
                    | (static_cast<std::uint32_t>(pix[off + 3]) << 24);
            }
            stbi_image_free(pix);
            return r;
        }

        static int make_stream(const std::vector<std::uint8_t> &buf) {
            const int r = alloc_obj(k_obj_stream);
            host_obj *o = obj(r);
            o->bytes = buf;
            o->pos = 0;
            return r;
        }

        static int resource_stream(const std::string &path) {
            std::string rel = path;
            if (!rel.empty() && rel[0] == '/') {
                rel.erase(rel.begin());
            }
            std::vector<std::uint8_t> buf;
            if (!guest_read_any(g_pr, rel, buf)) {
                LOG_WARN(EMULATED_STDOUT, "[j9-nf] host-res-miss '{}'", path);
                return 0;
            }
            LOG_WARN(EMULATED_STDOUT, "[j9-nf] host-res '{}' bytes={}", path, buf.size());
            return make_stream(buf);
        }

        static int make_multi_array(const int *ds, int nd, int d) {
            if ((d < 0) || (d >= nd)) {
                return 0;
            }
            const int n = std::max(0, ds[d]);
            const int r = alloc_obj(k_obj_array);
            host_obj *o = obj(r);
            o->arr.assign(static_cast<unsigned>(n), 0);
            if (d + 1 < nd) {
                o->arr_is_ref = 1;
                for (int i = 0; i < n; ++i) {
                    o->arr[static_cast<unsigned>(i)] = make_multi_array(ds, nd, d + 1);
                }
            }
            return r;
        }

        static int argc_of(const std::string &sig) {
            int n = 0;
            if (sig.size() < 3) {
                return 0;
            }
            for (std::size_t i = 1; i < sig.size() && sig[i] != ')'; ++i) {
                if (sig[i] == 'L') {
                    ++n;
                    while ((i < sig.size()) && (sig[i] != ';')) {
                        ++i;
                    }
                } else if (sig[i] == '[') {
                    ++n;
                    while ((i < sig.size()) && (sig[i] == '[')) {
                        ++i;
                    }
                    if ((i < sig.size()) && (sig[i] == 'L')) {
                        while ((i < sig.size()) && (sig[i] != ';')) {
                            ++i;
                        }
                    }
                } else if ((sig[i] == 'J') || (sig[i] == 'D')) {
                    n += 2;
                } else {
                    ++n;
                }
            }
            return n;
        }

        static bool is_void(const std::string &sig) {
            const auto p = sig.rfind(')');
            return (p != std::string::npos) && (p + 1 < sig.size()) && (sig[p + 1] == 'V');
        }

        static cf_method *find_method(cf_class *c, const std::string &name, const std::string &sig) {
            while (c) {
                for (auto &m : c->methods) {
                    if ((utf(c, m.name) == name) && (utf(c, m.desc) == sig)) {
                        return &m;
                    }
                }
                if (c->super.empty() || (c->super == "java/lang/Object")) {
                    break;
                }
                c = load_class(g_pr, c->super);
            }
            return nullptr;
        }

        static std::int32_t run_java(cf_class *c, cf_method *m, int this_ref, const std::int32_t *args, int nargs, int depth);

        static int arg_i(std::int32_t *args, int nargs, int i) {
            return ((i >= 0) && (i < nargs) && args) ? args[i] : 0;
        }

        static host_obj *stream_of(int ref) {
            host_obj *o = obj(ref);
            if (!o) {
                return nullptr;
            }
            if (o->kind == k_obj_stream) {
                return o;
            }
            if (o->nested) {
                return stream_of(o->nested);
            }
            return nullptr;
        }

        static bool native_invoke(const std::string &cls, const std::string &name, const std::string &sig,
            int this_ref, std::int32_t *args, int nargs, std::int32_t &ret, int depth) {
            (void)depth;
            ret = 0;
            if ((name == "<init>") && ((cls == "java/lang/Object")
                    || (cls.rfind("javax/microedition/", 0) == 0)
                    || (cls == "com/nokia/mid/ui/FullCanvas")
                    || (cls.rfind("com/nokia/mid/ui/", 0) == 0))) {
                return true;
            }
            if ((cls == "java/lang/StringBuffer") || (cls == "java/lang/StringBuilder")) {
                host_obj *o = obj(this_ref);
                if (!o) {
                    return true;
                }
                if (name == "<init>") {
                    o->kind = k_obj_sb;
                    o->str.clear();
                    return true;
                }
                if (name == "append") {
                    if (nargs >= 1) {
                        if (sig.find("Ljava/lang/String;") != std::string::npos) {
                            o->str += as_str(args[0]);
                        } else if (sig.find("Ljava/lang/Object;") != std::string::npos) {
                            o->str += as_str(args[0]);
                        } else {
                            o->str += std::to_string(args[0]);
                        }
                    }
                    ret = this_ref;
                    return true;
                }
                if (name == "toString") {
                    ret = intern(o->str);
                    return true;
                }
            }
            if (cls == "java/lang/String") {
                host_obj *so = obj(this_ref);
                if (name == "<init>") {
                    if (so && (sig.find("[B") != std::string::npos)) {
                        host_obj *arr = obj(arg_i(args, nargs, 0));
                        int off = 0;
                        int len = 0;
                        if (sig.find("II") != std::string::npos) {
                            off = arg_i(args, nargs, 1);
                            len = arg_i(args, nargs, 2);
                        } else if (arr) {
                            len = static_cast<int>(arr->arr.size());
                        }
                        std::string s;
                        if (arr && (off >= 0) && (len > 0)
                            && (static_cast<unsigned>(off + len) <= arr->arr.size())) {
                            s.resize(static_cast<unsigned>(len));
                            for (int i = 0; i < len; ++i) {
                                s[static_cast<unsigned>(i)] = static_cast<char>(arr->arr[static_cast<unsigned>(off + i)] & 255);
                            }
                        }
                        so->kind = k_obj_string;
                        so->str = s;
                    } else if (so && (nargs >= 1)) {
                        so->kind = k_obj_string;
                        so->str = as_str(args[0]);
                    }
                    return true;
                }
                if (name == "length") {
                    ret = utf8_len(as_str(this_ref));
                    return true;
                }
                if (name == "equals") {
                    ret = (as_str(this_ref) == as_str(arg_i(args, nargs, 0))) ? 1 : 0;
                    return true;
                }
                if (name == "valueOf") {
                    ret = intern(std::to_string(arg_i(args, nargs, 0)));
                    return true;
                }
                if (name == "charAt") {
                    ret = static_cast<std::int32_t>(utf8_at(as_str(this_ref), arg_i(args, nargs, 0)));
                    return true;
                }
                if (name == "substring") {
                    const std::string s = as_str(this_ref);
                    const int n = utf8_len(s);
                    int a = std::max(0, arg_i(args, nargs, 0));
                    int b = (nargs >= 2) ? arg_i(args, nargs, 1) : n;
                    a = std::min(a, n);
                    b = std::max(a, std::min(b, n));
                    ret = intern(utf8_substr(s, a, b));
                    return true;
                }
                if (name == "trim") {
                    std::string s = as_str(this_ref);
                    while (!s.empty() && static_cast<unsigned char>(s.front()) <= 32) {
                        s.erase(s.begin());
                    }
                    while (!s.empty() && static_cast<unsigned char>(s.back()) <= 32) {
                        s.pop_back();
                    }
                    ret = intern(s);
                    return true;
                }
            }
            if ((cls == "java/lang/Integer") && (name == "parseInt")) {
                try {
                    ret = std::stoi(as_str(arg_i(args, nargs, 0)));
                } catch (...) {
                    ret = 0;
                }
                return true;
            }
            if (cls == "java/lang/Math") {
                if (name == "abs") {
                    const int v = arg_i(args, nargs, 0);
                    ret = (v < 0) ? -v : v;
                    return true;
                }
                if (name == "min") {
                    ret = std::min(arg_i(args, nargs, 0), arg_i(args, nargs, 1));
                    return true;
                }
                if (name == "max") {
                    ret = std::max(arg_i(args, nargs, 0), arg_i(args, nargs, 1));
                    return true;
                }
            }
            if (cls == "java/lang/System") {
                if (name == "arraycopy") {
                    host_obj *src = obj(arg_i(args, nargs, 0));
                    host_obj *dst = obj(arg_i(args, nargs, 2));
                    const int sp = arg_i(args, nargs, 1);
                    const int dp = arg_i(args, nargs, 3);
                    const int n = arg_i(args, nargs, 4);
                    if (src && dst && (n > 0)) {
                        for (int i = 0; i < n; ++i) {
                            const int si = sp + i;
                            const int di = dp + i;
                            if ((si >= 0) && (di >= 0)
                                && (static_cast<unsigned>(si) < src->arr.size())
                                && (static_cast<unsigned>(di) < dst->arr.size())) {
                                dst->arr[static_cast<unsigned>(di)] = src->arr[static_cast<unsigned>(si)];
                            }
                        }
                    }
                    return true;
                }
                if (name == "gc" || name == "currentTimeMillis") {
                    ret = 0;
                    return true;
                }
            }
            if (cls == "java/io/PrintStream") {
                if ((name == "println") || (name == "print")) {
                    LOG_WARN(EMULATED_STDOUT, "[j9-nf] host-out '{}'", as_str(arg_i(args, nargs, 0)));
                    return true;
                }
            }
            if (cls == "java/lang/Object") {
                if (name == "getClass") {
                    const int r = alloc_obj(k_obj_class);
                    host_obj *th = obj(this_ref);
                    obj(r)->str = (th && th->clazz) ? th->clazz->name : "java/lang/Object";
                    ret = r;
                    return true;
                }
                if (name == "toString" || name == "hashCode" || name == "equals" || name == "notify"
                    || name == "notifyAll" || name == "wait") {
                    ret = (name == "toString") ? intern("obj") : 0;
                    return true;
                }
            }
            if (cls == "java/lang/Class") {
                if (name == "getResourceAsStream") {
                    ret = resource_stream(as_str(arg_i(args, nargs, 0)));
                    return true;
                }
                if (name == "forName") {
                    ret = intern(as_str(arg_i(args, nargs, 0)));
                    return true;
                }
            }
            if (cls == "java/lang/Runtime") {
                if (name == "getRuntime") {
                    static int rt = 0;
                    if (!rt) {
                        rt = alloc_obj(k_obj_java);
                    }
                    ret = rt;
                    return true;
                }
                if (name == "gc" || name == "freeMemory" || name == "totalMemory") {
                    ret = 1024 * 1024;
                    return true;
                }
            }
            if (cls == "java/lang/Thread") {
                host_obj *th = obj(this_ref);
                if (name == "<init>") {
                    if (th) {
                        th->kind = k_obj_thread;
                        th->nested = arg_i(args, nargs, 0);
                    }
                    return true;
                }
                if (name == "start" || name == "yield" || name == "sleep" || name == "interrupt"
                    || name == "join" || name == "setPriority") {
                    return true;
                }
                if (name == "currentThread") {
                    ret = this_ref ? this_ref : alloc_obj(k_obj_thread);
                    return true;
                }
            }
            if (cls.find("$Sound") != std::string::npos) {
                return true;
            }
            if (cls == "java/lang/Throwable") {
                if (name == "printStackTrace" || name == "<init>" || name == "toString"
                    || name == "getMessage") {
                    return true;
                }
            }
            if (cls == "java/util/Random") {
                host_obj *o = obj(this_ref);
                if (name == "<init>") {
                    if (o) {
                        o->kind = k_obj_random;
                        o->seed = static_cast<int>(std::time(nullptr) | 1);
                    }
                    return true;
                }
                if (name == "nextInt") {
                    if (o) {
                        o->seed = o->seed * 1103515245 + 12345;
                        ret = (o->seed >> 16) & 0x7fffffff;
                    }
                    return true;
                }
            }
            if (cls == "java/util/Vector") {
                host_obj *o = obj(this_ref);
                if (name == "<init>") {
                    if (o) {
                        o->kind = k_obj_vector;
                        o->arr.clear();
                        o->arr_is_ref = 1;
                    }
                    return true;
                }
                if (!o) {
                    return true;
                }
                if (name == "addElement") {
                    o->arr.push_back(arg_i(args, nargs, 0));
                    return true;
                }
                if (name == "elementAt") {
                    const int i = arg_i(args, nargs, 0);
                    ret = ((i >= 0) && (static_cast<unsigned>(i) < o->arr.size()))
                        ? o->arr[static_cast<unsigned>(i)] : 0;
                    return true;
                }
                if (name == "size") {
                    ret = static_cast<std::int32_t>(o->arr.size());
                    return true;
                }
                if (name == "removeAllElements") {
                    o->arr.clear();
                    return true;
                }
            }
            if ((cls == "java/io/InputStream") || (cls == "java/io/DataInputStream")
                || (cls == "java/io/ByteArrayInputStream")) {
                host_obj *st = stream_of(this_ref);
                if (name == "<init>") {
                    host_obj *self = obj(this_ref);
                    if (self) {
                        self->kind = k_obj_stream;
                        host_obj *src = obj(arg_i(args, nargs, 0));
                        if (src && (src->kind == k_obj_stream)) {
                            self->bytes = src->bytes;
                            self->pos = src->pos;
                            self->nested = arg_i(args, nargs, 0);
                        } else if (src && (src->kind == k_obj_array)) {
                            self->bytes.resize(src->arr.size());
                            for (std::size_t i = 0; i < src->arr.size(); ++i) {
                                self->bytes[i] = static_cast<std::uint8_t>(src->arr[i] & 255);
                            }
                            self->pos = 0;
                        }
                    }
                    return true;
                }
                if (!st) {
                    if (name == "read") {
                        ret = -1;
                    }
                    return true;
                }
                if (name == "available") {
                    ret = std::max(0, static_cast<int>(st->bytes.size()) - st->pos);
                    return true;
                }
                if (name == "close") {
                    return true;
                }
                if (name == "skip") {
                    const int n = (nargs >= 2) ? args[1] : arg_i(args, nargs, 0);
                    const int left = std::max(0, static_cast<int>(st->bytes.size()) - st->pos);
                    const int sk = std::max(0, std::min(n, left));
                    st->pos += sk;
                    ret = sk;
                    return true;
                }
                if (name == "read") {
                    if (sig == "()I") {
                        if (st->pos >= static_cast<int>(st->bytes.size())) {
                            ret = -1;
                        } else {
                            ret = st->bytes[static_cast<unsigned>(st->pos++)];
                        }
                        return true;
                    }
                    host_obj *dst = obj(arg_i(args, nargs, 0));
                    int off = 0;
                    int len = 0;
                    if (sig.find("II") != std::string::npos) {
                        off = arg_i(args, nargs, 1);
                        len = arg_i(args, nargs, 2);
                    } else if (dst) {
                        len = static_cast<int>(dst->arr.size());
                    }
                    int n = 0;
                    if (dst && (len > 0)) {
                        while ((n < len) && (st->pos < static_cast<int>(st->bytes.size()))
                            && (static_cast<unsigned>(off + n) < dst->arr.size())) {
                            dst->arr[static_cast<unsigned>(off + n)] = st->bytes[static_cast<unsigned>(st->pos++)];
                            ++n;
                        }
                    }
                    ret = n ? n : ((st->pos >= static_cast<int>(st->bytes.size())) ? -1 : 0);
                    return true;
                }
            }
            if ((cls == "java/io/OutputStream") || (cls == "java/io/DataOutputStream")
                || (cls == "java/io/ByteArrayOutputStream")) {
                if (name == "<init>" || name == "write" || name == "close" || name == "flush"
                    || name == "toByteArray") {
                    if (name == "toByteArray") {
                        ret = alloc_obj(k_obj_array);
                    }
                    return true;
                }
            }
            if (cls == "javax/microedition/rms/RecordStore") {
                if (name == "openRecordStore") {
                    const int r = alloc_obj(k_obj_rms);
                    obj(r)->str = as_str(arg_i(args, nargs, 0));
                    ret = r;
                    return true;
                }
                if (name == "getNumRecords") {
                    ret = 0;
                    return true;
                }
                if (name == "closeRecordStore" || name == "addRecord" || name == "setRecord") {
                    return true;
                }
                if (name == "getRecord") {
                    ret = 0;
                    return true;
                }
            }
            if (cls == "com/nokia/mid/ui/DeviceControl") {
                return true;
            }
            if (cls == "javax/microedition/lcdui/Display") {
                if (name == "getDisplay") {
                    if (!g_display) {
                        g_display = alloc_obj(k_obj_display);
                    }
                    ret = g_display;
                    return true;
                }
                if (name == "setCurrent") {
                    g_current = nargs ? args[0] : 0;
                    LOG_WARN(EMULATED_STDOUT, "[j9-nf] host-setCurrent 0x{:X}", g_current);
                    return true;
                }
                if (name == "getCurrent") {
                    ret = g_current;
                    return true;
                }
                if (name == "callSerially") {
                    return true;
                }
                if (name == "isColor" || name == "numColors") {
                    ret = (name == "isColor") ? 1 : 65536;
                    return true;
                }
            }
            if ((cls == "javax/microedition/lcdui/Canvas")
                || (cls == "javax/microedition/lcdui/Displayable")
                || (cls == "javax/microedition/lcdui/GameCanvas")
                || (cls == "com/nokia/mid/ui/FullCanvas")) {
                if ((name == "getWidth") || (name == "getHeight")) {
                    ret = (name == "getWidth") ? k_lcd_w : k_lcd_h;
                    return true;
                }
                if (name == "repaint" || name == "serviceRepaints" || name == "setFullScreenMode"
                    || name == "setTitle") {
                    return true;
                }
                if (name == "getGraphics") {
                    const int g = alloc_obj(k_obj_graphics);
                    host_obj *go = obj(g);
                    go->target = ensure_fb();
                    go->color = pack_rgba(0);
                    go->cw = k_lcd_w;
                    go->ch = k_lcd_h;
                    ret = g;
                    return true;
                }
                if (name == "flushGraphics") {
                    present();
                    return true;
                }
            }
            if (cls == "javax/microedition/lcdui/Graphics") {
                host_obj *g = obj(this_ref);
                if (!g) {
                    return true;
                }
                if (name == "setColor") {
                    if (nargs >= 3) {
                        g->color = static_cast<int>(pack_rgba(((args[0] & 255) << 16)
                            | ((args[1] & 255) << 8) | (args[2] & 255)));
                    } else {
                        g->color = static_cast<int>(pack_rgba(arg_i(args, nargs, 0)));
                    }
                    return true;
                }
                if (name == "setClip") {
                    if (nargs >= 4) {
                        g->cx = args[0];
                        g->cy = args[1];
                        g->cw = args[2];
                        g->ch = args[3];
                    }
                    return true;
                }
                if (name == "translate") {
                    if (nargs >= 2) {
                        g->tx += args[0];
                        g->ty += args[1];
                    }
                    return true;
                }
                if ((name == "fillRect") || (name == "fillRoundRect") || (name == "fillArc")) {
                    if (nargs >= 4) {
                        fill_rect(g, args[0], args[1], args[2], args[3]);
                    }
                    return true;
                }
                if ((name == "drawRect") || (name == "drawLine") || (name == "drawRoundRect")
                    || (name == "drawArc")) {
                    if ((name == "drawLine") && (nargs >= 4)) {
                        fill_rect(g, args[0], args[1], 1, 1);
                        fill_rect(g, args[2], args[3], 1, 1);
                    } else if (nargs >= 4) {
                        fill_rect(g, args[0], args[1], args[2], 1);
                        fill_rect(g, args[0], args[1] + args[3] - 1, args[2], 1);
                        fill_rect(g, args[0], args[1], 1, args[3]);
                        fill_rect(g, args[0] + args[2] - 1, args[1], 1, args[3]);
                    }
                    return true;
                }
                if (name == "drawImage") {
                    host_obj *im = (nargs >= 1) ? obj(args[0]) : nullptr;
                    int x = (nargs >= 2) ? args[1] : 0;
                    int y = (nargs >= 3) ? args[2] : 0;
                    const int anc = (nargs >= 4) ? args[3] : 0;
                    if (im) {
                        if (anc & 1) {
                            x -= im->w / 2;
                        }
                        if (anc & 8) {
                            x -= im->w;
                        }
                        if (anc & 2) {
                            y -= im->h / 2;
                        }
                        if (anc & 32) {
                            y -= im->h;
                        }
                        host_obj *dst = g->target ? obj(g->target) : fb();
                        blit(dst, x + g->tx, y + g->ty, im, 0, 0, im->w, im->h);
                    }
                    return true;
                }
                if (name == "drawRGB") {
                    host_obj *rgb = obj(arg_i(args, nargs, 0));
                    const int offset = arg_i(args, nargs, 1);
                    const int scan = arg_i(args, nargs, 2);
                    const int x = arg_i(args, nargs, 3);
                    const int y = arg_i(args, nargs, 4);
                    const int w = arg_i(args, nargs, 5);
                    const int h = arg_i(args, nargs, 6);
                    host_obj *dst = g->target ? obj(g->target) : fb();
                    if (rgb && dst && (w > 0) && (h > 0)) {
                        for (int yy = 0; yy < h; ++yy) {
                            for (int xx = 0; xx < w; ++xx) {
                                const int si = offset + yy * (scan ? scan : w) + xx;
                                if ((si < 0) || (static_cast<unsigned>(si) >= rgb->arr.size())) {
                                    continue;
                                }
                                const int dx = x + g->tx + xx;
                                const int dy = y + g->ty + yy;
                                if ((dx < 0) || (dy < 0) || (dx >= dst->w) || (dy >= dst->h)) {
                                    continue;
                                }
                                dst->pixels[static_cast<unsigned>(dy * dst->w + dx)] = pack_rgba(rgb->arr[static_cast<unsigned>(si)]);
                            }
                        }
                    }
                    return true;
                }
                if ((name == "drawString") || (name == "drawSubstring")) {
                    std::string text = as_str(arg_i(args, nargs, 0));
                    int x = 0;
                    int y = 0;
                    int anc = 0;
                    if (name == "drawSubstring") {
                        const int off = arg_i(args, nargs, 1);
                        const int len = arg_i(args, nargs, 2);
                        text = utf8_substr(text, off, off + len);
                        x = arg_i(args, nargs, 3);
                        y = arg_i(args, nargs, 4);
                        anc = arg_i(args, nargs, 5);
                    } else {
                        x = arg_i(args, nargs, 1);
                        y = arg_i(args, nargs, 2);
                        anc = arg_i(args, nargs, 3);
                    }
                    draw_string(g, text, x, y, anc);
                    return true;
                }
                if (name == "setFont") {
                    return true;
                }
                if ((name == "getClipX") || (name == "getClipY") || (name == "getClipWidth")
                    || (name == "getClipHeight") || (name == "getTranslateX") || (name == "getTranslateY")) {
                    if (name == "getClipX") ret = g->cx;
                    else if (name == "getClipY") ret = g->cy;
                    else if (name == "getClipWidth") ret = g->cw;
                    else if (name == "getClipHeight") ret = g->ch;
                    else if (name == "getTranslateX") ret = g->tx;
                    else ret = g->ty;
                    return true;
                }
            }
            if (cls == "javax/microedition/lcdui/Image") {
                if (name == "createImage") {
                    if (sig == "(Ljava/lang/String;)Ljavax/microedition/lcdui/Image;") {
                        ret = load_image(as_str(nargs ? args[0] : 0));
                        return true;
                    }
                    if (sig == "(II)Ljavax/microedition/lcdui/Image;") {
                        const int w = nargs >= 1 ? args[0] : 1;
                        const int h = nargs >= 2 ? args[1] : 1;
                        ret = alloc_obj(k_obj_image);
                        host_obj *im = obj(ret);
                        im->w = std::max(1, w);
                        im->h = std::max(1, h);
                        im->pixels.assign(static_cast<unsigned>(im->w * im->h), pack_rgba(0));
                        return true;
                    }
                    if (sig == "([BII)Ljavax/microedition/lcdui/Image;") {
                        host_obj *arr = obj(arg_i(args, nargs, 0));
                        const int off = arg_i(args, nargs, 1);
                        const int len = arg_i(args, nargs, 2);
                        if (arr && (off >= 0) && (len > 0)
                            && (static_cast<unsigned>(off + len) <= arr->arr.size())) {
                            std::vector<std::uint8_t> buf(static_cast<unsigned>(len));
                            for (int i = 0; i < len; ++i) {
                                buf[static_cast<unsigned>(i)] = static_cast<std::uint8_t>(
                                    arr->arr[static_cast<unsigned>(off + i)] & 255);
                            }
                            ret = image_from_bytes(buf.data(), static_cast<int>(buf.size()));
                            if (!ret) {
                                LOG_WARN(EMULATED_STDOUT, "[j9-nf] host-image-bytes-fail len={}", len);
                                ret = load_image("");
                            }
                        } else {
                            ret = load_image("");
                        }
                        return true;
                    }
                    if (nargs >= 1) {
                        host_obj *src = obj(args[0]);
                        if (src && (src->kind == k_obj_image)) {
                            ret = alloc_obj(k_obj_image);
                            *obj(ret) = *src;
                            obj(ret)->kind = k_obj_image;
                            return true;
                        }
                    }
                    ret = load_image("");
                    return true;
                }
                if ((name == "getWidth") || (name == "getHeight")) {
                    host_obj *im = obj(this_ref);
                    ret = im ? ((name == "getWidth") ? im->w : im->h) : 0;
                    return true;
                }
                if (name == "getGraphics") {
                    const int g = alloc_obj(k_obj_graphics);
                    host_obj *go = obj(g);
                    go->target = this_ref;
                    host_obj *im = obj(this_ref);
                    go->cw = im ? im->w : k_lcd_w;
                    go->ch = im ? im->h : k_lcd_h;
                    ret = g;
                    return true;
                }
            }
            if (cls == "javax/microedition/lcdui/Font") {
                if (name == "getDefaultFont" || name == "getFont") {
                    if (!g_font) {
                        g_font = alloc_obj(k_obj_java);
                    }
                    ret = g_font;
                    return true;
                }
                if (name == "getHeight") {
                    ret = k_font_px;
                    return true;
                }
                if (name == "stringWidth") {
                    ret = measure_text(as_str(nargs ? args[0] : 0));
                    return true;
                }
                if (name == "charWidth") {
                    const int ch = arg_i(args, nargs, 0);
                    ret = glyph_of(static_cast<std::uint32_t>(ch)).adv;
                    if (!ret) {
                        ret = (ch < 0x80) ? (k_font_px / 2) : k_font_px;
                    }
                    return true;
                }
            }
            if ((cls == "javax/microedition/midlet/MIDlet") && (name == "getAppProperty" || name == "notifyDestroyed")) {
                ret = intern("");
                return true;
            }
            if (cls.rfind("java/", 0) == 0 || cls.rfind("javax/", 0) == 0) {
                static int unk = 0;
                if (unk < 24) {
                    ++unk;
                    LOG_WARN(EMULATED_STDOUT, "[j9-nf] host-native-skip {}.{}{}", cls, name, sig);
                }
                return true;
            }
            return false;
        }

        static std::int32_t run_java(cf_class *c, cf_method *m, int this_ref, const std::int32_t *args, int nargs, int depth) {
            if (!c || !m || m->code.empty() || (depth > k_max_depth)) {
                return 0;
            }
            const std::vector<std::uint8_t> &code = m->code;
            std::vector<std::int32_t> loc(std::max<unsigned>(m->max_locals, 16u), 0);
            std::vector<std::int32_t> st;
            st.reserve(std::max<unsigned>(m->max_stack, 8u) + 8u);
            int ai = 0;
            unsigned li = 0;
            if ((m->flags & 0x8) == 0) {
                if (li < loc.size()) {
                    loc[li++] = this_ref;
                }
            }
            while ((ai < nargs) && (li < loc.size())) {
                loc[li++] = args[ai++];
            }
            auto push = [&](std::int32_t v) { st.push_back(v); };
            auto pop = [&]() -> std::int32_t {
                if (st.empty()) {
                    return 0;
                }
                const std::int32_t v = st.back();
                st.pop_back();
                return v;
            };
            unsigned pc = 0;
            int steps = 0;
            auto u8 = [&](unsigned o) { return (pc + o < code.size()) ? code[pc + o] : 0; };
            auto i8 = [&](unsigned o) { return static_cast<std::int8_t>(u8(o)); };
            auto i16 = [&](unsigned o) -> std::int16_t {
                return static_cast<std::int16_t>((u8(o) << 8) | u8(o + 1));
            };
            auto u16 = [&](unsigned o) -> unsigned { return (static_cast<unsigned>(u8(o)) << 8) | u8(o + 1); };
            auto i32at = [&](unsigned o) -> std::int32_t {
                return static_cast<std::int32_t>((u8(o) << 24) | (u8(o + 1) << 16) | (u8(o + 2) << 8) | u8(o + 3));
            };
            while ((pc < code.size()) && (steps++ < k_max_steps)) {
                const std::uint8_t op = code[pc];
                switch (op) {
                case 0x00:
                    pc += 1;
                    break;
                case 0x01:
                    push(0);
                    pc += 1;
                    break;
                case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08:
                    push(static_cast<std::int32_t>(op) - 3);
                    pc += 1;
                    break;
                case 0x09:
                    push(0);
                    push(0);
                    pc += 1;
                    break;
                case 0x0A:
                    push(0);
                    push(1);
                    pc += 1;
                    break;
                case 0x10:
                    push(i8(1));
                    pc += 2;
                    break;
                case 0x11:
                    push(i16(1));
                    pc += 3;
                    break;
                case 0x12: {
                    const unsigned idx = u8(1);
                    if ((idx < c->cp.size()) && (c->cp[idx].tag == k_string)) {
                        push(intern(cp_string(c, idx)));
                    } else if ((idx < c->cp.size()) && (c->cp[idx].tag == k_int)) {
                        push(c->cp[idx].i);
                    } else if ((idx < c->cp.size()) && (c->cp[idx].tag == k_class)) {
                        push(intern(cp_class_name(c, idx)));
                    } else {
                        push(0);
                    }
                    pc += 2;
                    break;
                }
                case 0x13: {
                    const unsigned idx = u16(1);
                    if ((idx < c->cp.size()) && (c->cp[idx].tag == k_string)) {
                        push(intern(cp_string(c, idx)));
                    } else if ((idx < c->cp.size()) && (c->cp[idx].tag == k_int)) {
                        push(c->cp[idx].i);
                    } else {
                        push(0);
                    }
                    pc += 3;
                    break;
                }
                case 0x14: {
                    const unsigned idx = u16(1);
                    std::int64_t v = 0;
                    if (idx < c->cp.size()) {
                        v = c->cp[idx].l;
                    }
                    push(static_cast<std::int32_t>(v >> 32));
                    push(static_cast<std::int32_t>(v));
                    pc += 3;
                    break;
                }
                case 0x15: case 0x17: case 0x19:
                    push((u8(1) < loc.size()) ? loc[u8(1)] : 0);
                    pc += 2;
                    break;
                case 0x16: case 0x18: {
                    const unsigned i = u8(1);
                    push((i < loc.size()) ? loc[i] : 0);
                    push(((i + 1) < loc.size()) ? loc[i + 1] : 0);
                    pc += 2;
                    break;
                }
                case 0x1A: case 0x1B: case 0x1C: case 0x1D:
                    push(loc[op - 0x1A]);
                    pc += 1;
                    break;
                case 0x1E: case 0x1F: case 0x20: case 0x21: {
                    const unsigned i = static_cast<unsigned>(op - 0x1E);
                    push((i < loc.size()) ? loc[i] : 0);
                    push(((i + 1) < loc.size()) ? loc[i + 1] : 0);
                    pc += 1;
                    break;
                }
                case 0x2A: case 0x2B: case 0x2C: case 0x2D:
                    push(loc[op - 0x2A]);
                    pc += 1;
                    break;
                case 0x36: case 0x37: case 0x38: case 0x39: case 0x3A: {
                    const unsigned i = u8(1);
                    if (op == 0x37) {
                        const std::int32_t lo = pop();
                        pop();
                        if (i < loc.size()) {
                            loc[i] = lo;
                        }
                    } else if (i < loc.size()) {
                        loc[i] = pop();
                    } else {
                        pop();
                    }
                    pc += 2;
                    break;
                }
                case 0x3B: case 0x3C: case 0x3D: case 0x3E:
                    loc[op - 0x3B] = pop();
                    pc += 1;
                    break;
                case 0x4B: case 0x4C: case 0x4D: case 0x4E:
                    loc[op - 0x4B] = pop();
                    pc += 1;
                    break;
                case 0x2E: case 0x32: case 0x33: case 0x34: case 0x35: {
                    const int idx = pop();
                    host_obj *a = obj(pop());
                    std::int32_t v = (a && (idx >= 0) && (static_cast<unsigned>(idx) < a->arr.size()))
                        ? a->arr[static_cast<unsigned>(idx)] : 0;
                    if (op == 0x33) {
                        v = static_cast<std::int8_t>(v & 255);
                    } else if (op == 0x34) {
                        v = v & 0xffff;
                    } else if (op == 0x35) {
                        v = static_cast<std::int16_t>(v & 0xffff);
                    }
                    push(v);
                    pc += 1;
                    break;
                }
                case 0x4F: case 0x53: case 0x54: case 0x55: case 0x56: {
                    const int v = pop();
                    const int idx = pop();
                    host_obj *a = obj(pop());
                    if (a && (idx >= 0) && (static_cast<unsigned>(idx) < a->arr.size())) {
                        a->arr[static_cast<unsigned>(idx)] = v;
                    }
                    pc += 1;
                    break;
                }
                case 0x57:
                    pop();
                    pc += 1;
                    break;
                case 0x58:
                    pop();
                    pop();
                    pc += 1;
                    break;
                case 0x59: {
                    const std::int32_t v = st.empty() ? 0 : st.back();
                    push(v);
                    pc += 1;
                    break;
                }
                case 0x5A: {
                    const std::int32_t a = pop();
                    const std::int32_t b = pop();
                    push(a);
                    push(b);
                    push(a);
                    pc += 1;
                    break;
                }
                case 0x5B: {
                    const std::int32_t a = pop();
                    const std::int32_t b = pop();
                    const std::int32_t c = pop();
                    push(a);
                    push(c);
                    push(b);
                    push(a);
                    pc += 1;
                    break;
                }
                case 0x5C: {
                    const std::int32_t a = pop();
                    const std::int32_t b = pop();
                    push(b);
                    push(a);
                    push(b);
                    push(a);
                    pc += 1;
                    break;
                }
                case 0x5D: {
                    const std::int32_t a = pop();
                    const std::int32_t b = pop();
                    const std::int32_t c = pop();
                    push(b);
                    push(a);
                    push(c);
                    push(b);
                    push(a);
                    pc += 1;
                    break;
                }
                case 0x5F: {
                    const std::int32_t a = pop();
                    const std::int32_t b = pop();
                    push(a);
                    push(b);
                    pc += 1;
                    break;
                }
                case 0x60: case 0x64: {
                    const std::int32_t b = pop();
                    const std::int32_t a = pop();
                    push((op == 0x60) ? (a + b) : (a - b));
                    pc += 1;
                    break;
                }
                case 0x68: {
                    const std::int32_t b = pop();
                    const std::int32_t a = pop();
                    push(a * b);
                    pc += 1;
                    break;
                }
                case 0x6C: case 0x70: {
                    const std::int32_t b = pop();
                    const std::int32_t a = pop();
                    push(b ? ((op == 0x6C) ? (a / b) : (a % b)) : 0);
                    pc += 1;
                    break;
                }
                case 0x74:
                    push(-pop());
                    pc += 1;
                    break;
                case 0x78: case 0x7A: case 0x7C: {
                    const std::int32_t b = pop();
                    const std::int32_t a = pop();
                    if (op == 0x78) push(a << (b & 31));
                    else if (op == 0x7A) push(a >> (b & 31));
                    else push(static_cast<std::int32_t>(static_cast<std::uint32_t>(a) >> (b & 31)));
                    pc += 1;
                    break;
                }
                case 0x7E: case 0x80: case 0x82: {
                    const std::int32_t b = pop();
                    const std::int32_t a = pop();
                    if (op == 0x7E) push(a & b);
                    else if (op == 0x80) push(a | b);
                    else push(a ^ b);
                    pc += 1;
                    break;
                }
                case 0x84: {
                    const unsigned i = u8(1);
                    if (i < loc.size()) {
                        loc[i] += i8(2);
                    }
                    pc += 3;
                    break;
                }
                case 0x85: {
                    const std::int32_t v = pop();
                    push(v < 0 ? -1 : 0);
                    push(v);
                    pc += 1;
                    break;
                }
                case 0x88: {
                    const std::int32_t lo = pop();
                    pop();
                    push(lo);
                    pc += 1;
                    break;
                }
                case 0x91:
                    push(static_cast<std::int8_t>(pop() & 255));
                    pc += 1;
                    break;
                case 0x92:
                    push(pop() & 0xffff);
                    pc += 1;
                    break;
                case 0x93:
                    push(static_cast<std::int16_t>(pop() & 0xffff));
                    pc += 1;
                    break;
                case 0x94: case 0x95: case 0x96: case 0x97: case 0x98: {
                    const std::int32_t b = pop();
                    const std::int32_t a = pop();
                    push((a < b) ? -1 : ((a == b) ? 0 : 1));
                    pc += 1;
                    break;
                }
                case 0x99: case 0x9A: case 0x9B: case 0x9C: case 0x9D: case 0x9E: {
                    const std::int32_t v = pop();
                    bool t = false;
                    if (op == 0x99) t = (v == 0);
                    else if (op == 0x9A) t = (v != 0);
                    else if (op == 0x9B) t = (v < 0);
                    else if (op == 0x9C) t = (v >= 0);
                    else if (op == 0x9D) t = (v > 0);
                    else t = (v <= 0);
                    pc = t ? static_cast<unsigned>(static_cast<int>(pc) + i16(1)) : (pc + 3);
                    break;
                }
                case 0x9F: case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5: case 0xA6: {
                    const std::int32_t b = pop();
                    const std::int32_t a = pop();
                    bool t = false;
                    if (op == 0x9F) t = (a == b);
                    else if (op == 0xA0) t = (a != b);
                    else if (op == 0xA1) t = (a < b);
                    else if (op == 0xA2) t = (a >= b);
                    else if (op == 0xA3) t = (a > b);
                    else if (op == 0xA4) t = (a <= b);
                    else if (op == 0xA5) t = (a == b);
                    else t = (a != b);
                    pc = t ? static_cast<unsigned>(static_cast<int>(pc) + i16(1)) : (pc + 3);
                    break;
                }
                case 0xA7:
                    pc = static_cast<unsigned>(static_cast<int>(pc) + i16(1));
                    break;
                case 0xAA: {
                    const unsigned origin = pc;
                    unsigned p = pc + 1;
                    while ((p & 3u) != 0) {
                        ++p;
                    }
                    const std::int32_t def = i32at(p - pc);
                    const std::int32_t low = i32at(p + 4 - pc);
                    const std::int32_t high = i32at(p + 8 - pc);
                    const std::int32_t key = pop();
                    std::int32_t off = def;
                    if ((key >= low) && (key <= high)) {
                        off = i32at(p + 12 + static_cast<unsigned>((key - low) * 4) - pc);
                    }
                    pc = static_cast<unsigned>(static_cast<int>(origin) + off);
                    break;
                }
                case 0xAB: {
                    const unsigned origin = pc;
                    unsigned p = pc + 1;
                    while ((p & 3u) != 0) {
                        ++p;
                    }
                    const std::int32_t def = i32at(p - pc);
                    const std::int32_t npairs = i32at(p + 4 - pc);
                    const std::int32_t key = pop();
                    std::int32_t off = def;
                    for (std::int32_t i = 0; i < npairs; ++i) {
                        const unsigned e = p + 8 + static_cast<unsigned>(i * 8);
                        const std::int32_t match = i32at(e - pc);
                        if (match == key) {
                            off = i32at(e + 4 - pc);
                            break;
                        }
                    }
                    pc = static_cast<unsigned>(static_cast<int>(origin) + off);
                    break;
                }
                case 0xAC: case 0xB0:
                    return pop();
                case 0xB1:
                    return 0;
                case 0xB2: case 0xB3: {
                    std::string cls, name, sig;
                    cp_method(c, u16(1), cls, name, sig);
                    cf_class *oc = load_class(g_pr, cls);
                    if (op == 0xB2) {
                        std::int32_t v = 0;
                        if (oc) {
                            auto it = oc->statics.find(name);
                            if (it != oc->statics.end()) {
                                v = it->second;
                            }
                        }
                        push(v);
                    } else {
                        const std::int32_t v = pop();
                        if (oc) {
                            oc->statics[name] = v;
                        }
                    }
                    pc += 3;
                    break;
                }
                case 0xB4: case 0xB5: {
                    std::string cls, name, sig;
                    cp_method(c, u16(1), cls, name, sig);
                    if (op == 0xB5) {
                        const std::int32_t v = pop();
                        host_obj *o = obj(pop());
                        if (o) {
                            o->fields[name] = v;
                        }
                    } else {
                        host_obj *o = obj(pop());
                        push(o ? o->fields[name] : 0);
                    }
                    pc += 3;
                    break;
                }
                case 0xB6: case 0xB7: case 0xB8: case 0xB9: {
                    std::string cls, name, sig;
                    cp_method(c, u16(1), cls, name, sig);
                    const int n = argc_of(sig);
                    std::vector<std::int32_t> av(static_cast<unsigned>(std::max(n, 0)));
                    for (int i = n - 1; i >= 0; --i) {
                        av[static_cast<unsigned>(i)] = pop();
                    }
                    int th = 0;
                    if (op != 0xB8) {
                        th = pop();
                    }
                    std::int32_t rv = 0;
                    if (!native_invoke(cls, name, sig, th, av.empty() ? nullptr : av.data(), n, rv, depth)) {
                        cf_class *tc = load_class(g_pr, cls);
                        if (!tc && th) {
                            host_obj *o = obj(th);
                            tc = o ? o->clazz : nullptr;
                        }
                        cf_method *tm = tc ? find_method(tc, name, sig) : nullptr;
                        if (tm) {
                            rv = run_java(tc, tm, th, av.empty() ? nullptr : av.data(), n, depth + 1);
                        } else if (!cls.empty()) {
                            static int miss = 0;
                            if (miss < 20) {
                                ++miss;
                                LOG_WARN(EMULATED_STDOUT, "[j9-nf] host-invoke-miss {}.{}{}", cls, name, sig);
                            }
                        }
                    }
                    if (!is_void(sig)) {
                        push(rv);
                    }
                    pc += (op == 0xB9) ? 5 : 3;
                    break;
                }
                case 0xBB: {
                    const std::string nm = cp_class_name(c, u16(1));
                    int r = 0;
                    if (nm == "java/lang/StringBuffer" || nm == "java/lang/StringBuilder") {
                        r = alloc_obj(k_obj_sb);
                    } else if (nm == "java/lang/String") {
                        r = intern("");
                    } else {
                        r = alloc_obj(k_obj_java);
                        host_obj *o = obj(r);
                        o->clazz = load_class(g_pr, nm);
                    }
                    push(r);
                    pc += 3;
                    break;
                }
                case 0xBC: {
                    const int n = pop();
                    const int r = alloc_obj(k_obj_array);
                    obj(r)->arr.assign(static_cast<unsigned>(std::max(n, 0)), 0);
                    push(r);
                    pc += 2;
                    break;
                }
                case 0xBD: {
                    const int n = pop();
                    const int r = alloc_obj(k_obj_array);
                    obj(r)->arr.assign(static_cast<unsigned>(std::max(n, 0)), 0);
                    obj(r)->arr_is_ref = 1;
                    push(r);
                    pc += 3;
                    break;
                }
                case 0xBE: {
                    host_obj *a = obj(pop());
                    push(a ? static_cast<std::int32_t>(a->arr.size()) : 0);
                    pc += 1;
                    break;
                }
                case 0xBF:
                    LOG_WARN(EMULATED_STDOUT, "[j9-nf] host-athrow in {}", c->name);
                    return 0;
                case 0xC0: case 0xC1:
                    if (op == 0xC1) {
                        push(pop() ? 1 : 0);
                    }
                    pc += 3;
                    break;
                case 0xC2: case 0xC3:
                    if (op == 0xC2) {
                        pop();
                    } else {
                        pop();
                    }
                    pc += 1;
                    break;
                case 0xC5: {
                    const unsigned dims = u8(3);
                    std::vector<int> ds(dims);
                    for (int i = static_cast<int>(dims) - 1; i >= 0; --i) {
                        ds[static_cast<unsigned>(i)] = pop();
                    }
                    push(dims ? make_multi_array(ds.data(), static_cast<int>(dims), 0) : 0);
                    pc += 4;
                    break;
                }
                case 0xC6: case 0xC7: {
                    const std::int32_t v = pop();
                    const bool t = (op == 0xC6) ? (v == 0) : (v != 0);
                    pc = t ? static_cast<unsigned>(static_cast<int>(pc) + i16(1)) : (pc + 3);
                    break;
                }
                default:
                    static int unk = 0;
                    if (unk < 16) {
                        ++unk;
                        LOG_WARN(EMULATED_STDOUT, "[j9-nf] host-op-skip 0x{:02X} pc={} {}", op, pc, c->name);
                    }
                    pc += 1;
                    break;
                }
            }
            if (steps >= k_max_steps) {
                LOG_WARN(EMULATED_STDOUT, "[j9-nf] host-step-limit {} pc={} stack={}", c->name, pc, st.size());
            }
            return st.empty() ? 0 : st.back();
        }

        static void call_named(cf_class *c, int this_ref, const char *name, const char *sig) {
            if (!c) {
                return;
            }
            cf_method *m = find_method(c, name, sig);
            if (!m) {
                return;
            }
            LOG_WARN(EMULATED_STDOUT, "[j9-nf] host-call {}.{}{}", c->name, name, sig);
            run_java(c, m, this_ref, nullptr, 0, 0);
        }

        static void paint_current() {
            host_obj *cur = obj(g_current);
            if (!cur || !cur->clazz) {
                return;
            }
            const int g = alloc_obj(k_obj_graphics);
            host_obj *go = obj(g);
            go->target = ensure_fb();
            go->color = pack_rgba(0);
            go->cw = k_lcd_w;
            go->ch = k_lcd_h;
            std::int32_t arg = g;
            cf_method *m = find_method(cur->clazz, "paint", "(Ljavax/microedition/lcdui/Graphics;)V");
            if (!m) {
                m = find_method(cur->clazz, "paint", "(Ljavax/microedition/lcdui/Graphics;)V");
            }
            if (m) {
                LOG_WARN(EMULATED_STDOUT, "[j9-nf] host-paint {}", cur->clazz->name);
                run_java(cur->clazz, m, g_current, &arg, 1, 0);
            } else {
                LOG_WARN(EMULATED_STDOUT, "[j9-nf] host-paint-miss {}", cur->clazz->name);
            }
            if (cf_method *show = find_method(cur->clazz, "showNotify", "()V")) {
                run_java(cur->clazz, show, g_current, nullptr, 0, 0);
            }
            if (cf_method *ms = find_method(cur->clazz, "MAIN_STATE", "()V")) {
                LOG_WARN(EMULATED_STDOUT, "[j9-nf] host-main-state {}", cur->clazz->name);
                for (int i = 0; i < 4; ++i) {
                    run_java(cur->clazz, ms, g_current, nullptr, 0, 0);
                }
            }
            present();
        }

        static void tick_frame() {
            host_obj *cur = obj(g_current);
            if (!cur || !cur->clazz) {
                return;
            }
            if (!cur->fields["g"]) {
                const int g = alloc_obj(k_obj_graphics);
                host_obj *go = obj(g);
                go->target = ensure_fb();
                go->color = pack_rgba(0);
                go->cw = k_lcd_w;
                go->ch = k_lcd_h;
                cur->fields["g"] = g;
            }
            if (cf_method *ms = find_method(cur->clazz, "MAIN_STATE", "()V")) {
                run_java(cur->clazz, ms, g_current, nullptr, 0, 0);
            }
            present();
        }

        static int map_scancode(int scancode) {
            switch (scancode) {
            case 0x10: return -1;  // up
            case 0x11: return -2;  // down
            case 0x0e: return -3;  // left
            case 0x0f: return -4;  // right
            case 0xa7: case 0xc4: return -5; // fire / call
            case 0xa4: return -6;  // left soft
            case 0xa5: case 0xc5: return -7; // right soft / end
            case 0x85: return 42;  // star
            case 0x7f: return 35;  // hash
            case 0x01: return 8;   // clear as fire-alt
            default:
                if ((scancode >= 0x30) && (scancode <= 0x39)) {
                    return scancode;
                }
                return 0;
            }
        }
    }

    static j9_ws_present_fn g_j9_ws_present = nullptr;

    void j9_register_ws_present(j9_ws_present_fn fn) {
        g_j9_ws_present = fn;
    }

    bool j9_present_surface(const std::uint32_t *rgba, int width, int height) {
        return g_j9_ws_present && rgba && (width > 0) && (height > 0)
            && g_j9_ws_present(rgba, width, height);
    }

    void j9_host_midp_reset() {
        g_classes.clear();
        g_objs.clear();
        g_intern.clear();
        g_display = 0;
        g_current = 0;
        g_fb = 0;
        g_font = 0;
        g_event_ready = false;
        g_pr = nullptr;
    }

    bool j9_host_midp_active() {
        return g_current != 0;
    }

    void j9_host_tick_midp() {
        if (!g_current || !g_event_ready) {
            return;
        }
        static int busy = 0;
        if (busy) {
            return;
        }
        static auto last = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - last < std::chrono::milliseconds(80)) {
            return;
        }
        last = now;
        busy = 1;
        tick_frame();
        busy = 0;
    }

    bool j9_host_key_event(int scancode, int pressed) {
        if (!g_current || !g_event_ready) {
            return false;
        }
        const int code = map_scancode(scancode);
        if (!code) {
            return true;
        }
        host_obj *cur = obj(g_current);
        if (!cur || !cur->clazz) {
            return true;
        }
        const char *nm = pressed ? "keyPressed" : "keyReleased";
        if (cf_method *m = find_method(cur->clazz, nm, "(I)V")) {
            std::int32_t arg = code;
            run_java(cur->clazz, m, g_current, &arg, 1, 0);
        }
        return true;
    }

    bool j9_host_run_midlet(kernel::process *pr, const char *main_class) {
        if (!pr || !main_class || !main_class[0]) {
            return false;
        }
        g_pr = pr;
        g_event_ready = true;
        ensure_fb();
        std::string name = main_class;
        for (char &c : name) {
            if (c == '.') {
                c = '/';
            }
        }
        cf_class *c = load_class(pr, name);
        if (!c) {
            LOG_WARN(EMULATED_STDOUT, "[j9-nf] host-midlet-miss '{}'", name);
            present();
            return false;
        }
        const int mid = alloc_obj(k_obj_java);
        obj(mid)->clazz = c;
        call_named(c, mid, "<init>", "()V");
        call_named(c, mid, "startApp", "()V");
        if (!g_current) {
            for (std::size_t i = 0; i < g_objs.size(); ++i) {
                host_obj *o = g_objs[i].get();
                if (o && o->clazz && (o->clazz->super.find("Canvas") != std::string::npos
                        || find_method(o->clazz, "paint", "(Ljavax/microedition/lcdui/Graphics;)V"))) {
                    g_current = static_cast<int>(i + 1);
                    break;
                }
            }
        }
        paint_current();
        LOG_WARN(EMULATED_STDOUT, "[j9-nf] host-midlet '{}' current={} objs={}",
            name, g_current, g_objs.size());
        return g_current != 0;
    }
}
