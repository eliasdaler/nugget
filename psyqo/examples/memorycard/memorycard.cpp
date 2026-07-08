/*

MIT License

Copyright (c) 2026 PCSX-Redux authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/

// A self-contained, interactive exercise of the psyqo memory card API. It is
// intentionally free of any engine code: just the GPU, a pad, and the
// MemoryCard / MemoryCardFileSystem classes.
//
// Each press of Cross performs exactly one action and appends its result to
// the on-screen log, so the whole API can be stepped through by hand.
//
// WARNING: the very first action FORMATS the memory card in port 1, erasing
// it. The opening screen makes this clear before anything happens.

#include "psyqo/advancedpad.hh"
#include "psyqo/application.hh"
#include "psyqo/font.hh"
#include "psyqo/gpu.hh"
#include "psyqo/memory-card-filesystem.hh"
#include "psyqo/memory-card.hh"
#include "psyqo/scene.hh"

#include "psyqo/xprintf.h"

#include "common/util/sjis-table.h"
#include "common/util/sjis-fullwidth-ascii.hh"
#include "common/util/sjis-decode.hh"

#include <cstdint>
#include <cstring>

#include <EASTL/string.h>
#include <EASTL/vector.h>

namespace
{

class MemoryCardExample final : public psyqo::Application {
    void prepare() override;
    void createScene() override;

public:
    psyqo::GPU::Configuration config;
    psyqo::Font<> m_font;
    psyqo::AdvancedPad m_input;
    psyqo::MemoryCard m_card;
    psyqo::MemoryCardFileSystem m_fs{m_card};
};

// A single scene that walks through the test steps, one per Cross press.
class MemoryCardScene final : public psyqo::Scene {
public:
    void frame() override;

    int m_index = 0; // next step to run
    bool m_started = false; // false -> showing the warning
    bool m_finished = false;
    bool m_prevCross = false;

    bool m_hirez = false;
    bool m_changeRes = false;
};

MemoryCardExample app;
MemoryCardScene scene;

struct SaveInfo {
    // icon
    int numFrames;
    int clutX;
    int clutY;
    int texX;
    int texY;
    eastl::string title;

    int currFrame{0};
    int currFrameDelay{0};
};

eastl::vector<SaveInfo> saves;

// Up to 20 characters, the Sony filename limit.
const char* const kFileName = "PSYQO-MEMCARD-TEST";

uint8_t g_payload[200];
constexpr uint32_t kPayloadLen = sizeof(g_payload);
uint8_t g_readback[8192 * 4];

// A simple 16x16 icon: a white border around a red fill.
void buildIcon(psyqo::MemoryCardFileSystem::Icon& icon)
{
    __builtin_memset(&icon, 0, sizeof(icon));
    icon.frameCount = 1;
    icon.clut[0] = 0x0000; // transparent
    icon.clut[1] = 0x7fff; // white border
    icon.clut[2] = 0x001f; // red fill
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x += 2) {
            uint8_t lo = (x == 0 || y == 0 || y == 15) ? 1 : 2;
            uint8_t hi = (x + 1 == 15 || y == 0 || y == 15) ? 1 : 2;
            icon.pixels[0][y * 8 + (x >> 1)] = lo | (hi << 4);
        }
    }
}

void drawSprite(psyqo::GPU& gpu,
    std::int16_t posX,
    std::int16_t posY,
    std::int16_t u,
    std::int16_t v,
    std::int16_t sx,
    std::int16_t sy,
    psyqo::Color color,
    psyqo::PrimPieces::TPageLoc tpage,
    psyqo::PrimPieces::ClutIndex clut)
{
    psyqo::Prim::TPage tpagePrim;
    tpagePrim.attr.set(psyqo::Prim::TPageAttr::ColorMode::Tex4Bits);
    tpagePrim.attr.setPageLoc(tpage);
    tpagePrim.attr.setDithering(false);
    gpu.sendPrimitive(tpagePrim);

    psyqo::Prim::Sprite sprite;
    sprite.texInfo.clut = clut;

    sprite.position.x = posX;
    sprite.position.y = posY;
    sprite.texInfo.u = u;
    sprite.texInfo.v = v;
    sprite.size.x = sx;
    sprite.size.y = sy;
    sprite.setColor(color);

    gpu.sendPrimitive(sprite);
}


} // namespace

void MemoryCardExample::prepare()
{
    config.set(psyqo::GPU::Resolution::W320)
        .set(psyqo::GPU::VideoMode::NTSC)
        .set(psyqo::GPU::ColorMode::C15BITS)
        .set(psyqo::GPU::Interlace::PROGRESSIVE);
    gpu().initialize(config);
#if 1
    m_card.prepare();
#endif
    
    m_input.setOnEvent([this](auto event) {
        if (event.type != psyqo::AdvancedPad::Event::ButtonReleased) return;
        scene.m_changeRes = true;
    });

}

void MemoryCardExample::createScene()
{
    // m_font.uploadSystemFont(gpu());
    m_font.uploadKromFont(gpu(), {{.x = 960, .y = int16_t(512 - 48 - 90)}});
    m_input.initialize();
    pushScene(&scene);

#if 1
    using MC = psyqo::MemoryCard;
    auto port = MC::Port::Port0;

    auto e = m_card.probeBlocking(port);
    if (e != MC::Error::OK) {
        printf("No card in port 1\n");
        return;
    }

    auto& fs = app.m_fs;
    psyqo::MemoryCardFileSystem::FileEntry entries[15];
    uint32_t count = 0;
    e = fs.listFilesBlocking(app.gpu(), port, entries, 15, &count);
    printf("num entries: %d\n", count);

    int offX = 640;

    int clutY = 0;
    int texX = offX+128;
    int texY = 0;

    psyqo::Vertex location{offX, 256};
    auto cursor = location;

    {
        psyqo::Prim::FastFill fill;
        fill.rect = {.pos = location, .size = {{.w = 256, .h = 256}}};
        gpu().sendPrimitive(fill);
    }


    for (int i = 0; i < count; i++) {
        uint32_t outLen = 0;
        e = fs.readFileBlocking(
            app.gpu(), port, entries[i].name, g_readback, sizeof(g_readback), &outLen);
        if (e != MC::Error::OK) {
            printf("Failed to read file %i\n", i);
            return;
        }

        printf("\nraw name:\n");
        for (int i = 0; i < 21; ++i) {
            printf("%d ", (uint8_t)entries[i].name[i]);
        }
        printf("\n");

        /* printf("\nraw data:\n");
        for (int i = 0; i < outLen; ++i) {
            printf("%d ", (uint8_t)g_readback[i]);
        }
        printf("\n");
        */

        char title[128];
        psyqo::MemoryCardFileSystem::FileInfo info;
        e = fs.readFileInfoBlocking(
            app.gpu(), port, entries[i].name, &info);
        Sjis::sjisTitleToAscii(title, 65, info.title, 65);
        // Sjis::sjisToUtf8(title, 128, info.title, 65);
        printf("title: %s\n", title);

#if 1
        {
            for (int ci = 0; ci < 32; ++ci) {
                uint16_t sjis = ((uint16_t*)info.title)[ci];
                sjis = (sjis >> 8) | (sjis << 8);
                psyqo::Prim::VRAMUpload upload;
                upload.region.pos = cursor;
                upload.region.size = {{.w = 4, .h = 15}};
                cursor.x += 4;
                if (cursor.x >= (location.x + 256)) {
                    cursor.x = location.x;
                    cursor.y += 15;
                }
                if (sjis == 0) {
                    continue;
                }
                const uint8_t* ptr = syscall_Krom2RawAdd(sjis);
                if (ptr == (const uint8_t*)-1) {
                    continue;
                }
                gpu().sendPrimitive(upload);
                for (unsigned i = 0; i < 15; i++) {
                    uint16_t v = ptr[0] | (ptr[1] << 8);
                    uint32_t d = 0;
                    for (unsigned j = 0; j < 16; j++) {
                        d <<= 4;
                        if (v & (1 << j)) {
                            d |= 1;
                        }
                        if ((j & 7) == 7) {
                            psyqo::Hardware::GPU::Data = d;
                        }
                    }
                    ptr += 2;
                }
            }
        }
#endif

        cursor.x = location.x;
        cursor.y += 15;

        printf("icon frame count: %d\n", info.icon.frameCount);

        const auto region = psyqo::Rect{
            .pos =
                {
                    .x = (std::int16_t)offX,
                    .y = (std::int16_t)clutY,
                },
            .size =
                {
                    .w = (std::int16_t)16,
                    .h = (std::int16_t)1,
                },
        };
        gpu().uploadToVRAM(info.icon.clut, region);

        texX = offX+64;
        for (int j = 0; j < info.icon.frameCount; ++j) {
            const auto region = psyqo::Rect{
                .pos =
                    {
                        .x = (std::int16_t)texX,
                        .y = (std::int16_t)texY,
                    },
                .size =
                    {
                        .w = (std::int16_t)4,
                        .h = (std::int16_t)16,
                    },
            };
            gpu().uploadToVRAM((const std::uint16_t*)info.icon.pixels[j], region);
            texX += 4;
        }

        saves.push_back(SaveInfo{
            .numFrames = info.icon.frameCount,
            .clutX = offX,
            .clutY = clutY,
            .texX = 0,
            .texY = texY,
            .title = title,
        });

        ++clutY;
        texY += 16;
    }

    gpu().waitChainIdle(); // wait for CLUT/icon uploads to finish
#endif
}

void MemoryCardScene::frame()
{
    if (m_changeRes) {
        m_changeRes = false;
        m_hirez = !m_hirez;
        if (m_hirez) {
            app.config.set(psyqo::GPU::Resolution::W640);
            app.config.set(psyqo::GPU::Interlace::INTERLACED);
        } else {
            app.config.set(psyqo::GPU::Resolution::W320);
            app.config.set(psyqo::GPU::Interlace::PROGRESSIVE);
        }
        gpu().reinitialize(app.config);
    }

    app.gpu().clear({{.r = 0, .g = 0, .b = 32}});

    static const auto white = psyqo::Color{{.r = 220, .g = 220, .b = 220}};
    static const auto gray = psyqo::Color{{.r = 150, .g = 150, .b = 150}};
    static const auto yellow = psyqo::Color{{.r = 255, .g = 230, .b = 0}};
    static const auto green = psyqo::Color{{.r = 0, .g = 230, .b = 0}};
    static const auto red = psyqo::Color{{.r = 255, .g = 40, .b = 40}};
    static const auto cyan = psyqo::Color{{.r = 0, .g = 200, .b = 230}};

    auto print = [](int16_t x, int16_t y, const psyqo::Color& c, const char* s) {
        app.m_font.print(app.gpu(), s, {{.x = x, .y = y}}, c);
    };

    // print(16, 16, yellow, "psyqo memory card test");
    // print(16, 32, white, "hello world");
    // print(16, 64, red, "okay...");

    psyqo::PrimPieces::TPageLoc loc;

#if 1
    loc.setPageX(10);
    loc.setPageY(1);
        drawSprite(gpu(),
                16, 128,
                0, 0,
                255, 255,
                // save.texX + save.currFrame * 16 + 16, save.texY + 16,
                psyqo::Color{255, 255, 255},
                loc,
                psyqo::PrimPieces::ClutIndex({.x = 640+64, .y=3}));

    loc.setPageX(11);
    loc.setPageY(1);
        drawSprite(gpu(),
                16+255, 128,
                0, 0,
                255, 255,
                // save.texX + save.currFrame * 16 + 16, save.texY + 16,
                psyqo::Color{255, 255, 255},
                loc,
                psyqo::PrimPieces::ClutIndex({.x = 640+64, .y=3}));
#endif

    loc.setPageX(11);
    loc.setPageY(0);
    int currY = 16;

    for (auto& save : saves) {
        drawSprite(gpu(),
                16, currY,
                save.texX + save.currFrame * 16, save.texY,
                16, 16,
                // save.texX + save.currFrame * 16 + 16, save.texY + 16,
                psyqo::Color{255, 255, 255},
                loc,
                psyqo::PrimPieces::ClutIndex({.x = save.clutX, .y=save.clutY}));

        print(16+32, currY, yellow, save.title.c_str());
        currY += 24;

        if (save.numFrames > 1) {
            if (save.currFrameDelay <= 0) {
                save.currFrameDelay = 10;
                ++save.currFrame;
                if (save.currFrame >= save.numFrames) {
                    save.currFrame = 0;
                }
            } else {
                --save.currFrameDelay;
            }
        }
    }
}

int main()
{
    return app.run();
}
