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
    psyqo::Font<> m_font;
    psyqo::AdvancedPad m_input;
    psyqo::MemoryCard m_card;
    psyqo::MemoryCardFileSystem m_fs{m_card};
};

// A single scene that walks through the test steps, one per Cross press.
class MemoryCardScene final : public psyqo::Scene {
    void frame() override;

    int m_index = 0; // next step to run
    bool m_started = false; // false -> showing the warning
    bool m_finished = false;
    bool m_prevCross = false;
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

// ── Result log ──────────────────────────────────────────────────────────

struct LogLine {
    const char* text;
    bool isResult; // true -> show a PASS/FAIL badge
    bool pass;
};

constexpr int kMaxLines = 24;
LogLine g_lines[kMaxLines];
int g_lineCount = 0;

void note(const char* text)
{
    if (g_lineCount < kMaxLines) g_lines[g_lineCount++] = LogLine{text, false, false};
}

// Records a pass/fail, and on failure adds the reason as its own line.
void check(const char* name, bool pass, const char* detail = nullptr)
{
    if (g_lineCount < kMaxLines) g_lines[g_lineCount++] = LogLine{name, true, pass};
    if (!pass && detail && g_lineCount < kMaxLines)
        g_lines[g_lineCount++] = LogLine{detail, false, false};
}

void checkOk(const char* name, psyqo::MemoryCard::Error error)
{
    bool ok = error == psyqo::MemoryCard::Error::OK;
    check(name, ok, ok ? nullptr : psyqo::MemoryCard::errorMessage(error));
}

// ── Test fixtures ───────────────────────────────────────────────────────

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


constexpr int kStepCount = 13;

// Performs one test action. Returns false if the run should stop (e.g. no card).
bool runStep(int index)
{
    using MC = psyqo::MemoryCard;
    auto port = MC::Port::Port0;
    auto& fs = app.m_fs;
    auto& card = app.m_card;
    uint32_t freeBlocks = 0;
    uint32_t outLen = 0;
    bool exists = false;
    MC::Error e;

    switch (index) {
        case 0:
            for (uint32_t i = 0; i < kPayloadLen; i++)
                g_payload[i] = static_cast<uint8_t>(i * 7 + 3);
            e = card.probeBlocking(port);
            checkOk("Card present", e);
            if (e != MC::Error::OK) {
                note("Insert a card in port 1, then reset.");
                return false;
            }
            return true;

        case 1:
            // checkOk("Format card", fs.formatBlocking(app.gpu(), port));
            return true;

        case 2:
            // checkOk("Card formatted", fs.getCardStateBlocking(app.gpu(), port));
            return true;

        case 3:
            e = fs.getFreeBlockCountBlocking(app.gpu(), port, &freeBlocks);
            check("Free blocks == 15",
                e == MC::Error::OK && freeBlocks == 15,
                e != MC::Error::OK ? MC::errorMessage(e) : "unexpected free count");
            return true;

        case 4:
        {
            // The title is a plain UTF-8 string; the driver encodes it to the
            // Shift-JIS field the BIOS shows (printable ASCII becomes fullwidth).
            psyqo::MemoryCardFileSystem::Icon icon;
            buildIcon(icon);
            checkOk("Write file",
                fs.writeFileBlocking(app.gpu(),
                    port,
                    kFileName,
                    "PSYQO MEMCARD TEST",
                    icon,
                    g_payload,
                    kPayloadLen));
            return true;
        }

        case 5:
            e = fs.fileExistsBlocking(app.gpu(), port, kFileName, &exists);
            check("File exists",
                e == MC::Error::OK && exists,
                e != MC::Error::OK ? MC::errorMessage(e) : "not found");
            return true;

        case 6:
            e = fs.readFileBlocking(
                app.gpu(), port, kFileName, g_readback, sizeof(g_readback), &outLen);
            check("Read + verify payload",
                e == MC::Error::OK && outLen >= kPayloadLen &&
                    __builtin_memcmp(g_readback, g_payload, kPayloadLen) == 0,
                e != MC::Error::OK ? MC::errorMessage(e) : "data mismatch");
            return true;

        case 7:
        {
            psyqo::MemoryCardFileSystem::FileEntry entries[15];
            uint32_t count = 0;
            e = fs.listFilesBlocking(app.gpu(), port, entries, 15, &count);
            bool listed = e == MC::Error::OK && count == 1;
            if (listed) {
                const char* n = entries[0].name;
                for (int i = 0; kFileName[i] || n[i]; i++) {
                    if (kFileName[i] != n[i]) {
                        listed = false;
                        break;
                    }
                }
            }
            check("List shows our file",
                listed,
                e != MC::Error::OK ? MC::errorMessage(e) : "wrong listing");
            return true;
        }

        case 8:
            e = fs.getFreeBlockCountBlocking(app.gpu(), port, &freeBlocks);
            check("Free blocks == 14",
                e == MC::Error::OK && freeBlocks == 14,
                e != MC::Error::OK ? MC::errorMessage(e) : "unexpected free count");
            return true;

        case 9:
            checkOk("Delete file", fs.deleteFileBlocking(app.gpu(), port, kFileName));
            return true;

        case 10:
            e = fs.fileExistsBlocking(app.gpu(), port, kFileName, &exists);
            check("File removed",
                e == MC::Error::OK && !exists,
                e != MC::Error::OK ? MC::errorMessage(e) : "still present");
            return true;

        case 11:
            e = fs.getFreeBlockCountBlocking(app.gpu(), port, &freeBlocks);
            check("Free blocks == 15",
                e == MC::Error::OK && freeBlocks == 15,
                e != MC::Error::OK ? MC::errorMessage(e) : "unexpected free count");
            return true;

        case 12:
            e = fs.readFileBlocking(
                app.gpu(), port, kFileName, g_readback, sizeof(g_readback), &outLen);
            check(
                "Missing file -> FileNotFound", e == MC::Error::FileNotFound, MC::errorMessage(e));
            note("Done.");
            return false;
    }
    return false;
}

eastl::string toUtf8(const eastl::string_view& str)
{
    eastl::string ret;
    constexpr unsigned tableSize =
        sizeof(c_sjisToUnicodeConvTable) / sizeof(c_sjisToUnicodeConvTable[0]);
    for (size_t i = 0; i < str.length(); i++) {
        uint8_t c = str[i];
        uint32_t index = 0;
        switch (c >> 4) {
            case 8:
                index = 0x100;
                break;
            case 9:
                index = 0x1100;
                break;
            case 14:
                index = 0x2100;
                break;
        }

        if (index != 0) {
            index += (c & 0x0f) << 8;
            i++;
            if (i >= str.length()) break;
            c = str[i];
        }

        index += c;

        if (index >= tableSize) continue;
        uint16_t v = c_sjisToUnicodeConvTable[index];
        if (v < 0x80) {
            ret += v;
        } else if (v < 0x800) {
            ret += 0xc0 | (v >> 6);
            ret += 0x80 | (v & 0x3f);
        } else {
            ret += 0xe0 | (v >> 12);
            ret += 0x80 | ((v & 0xfff) >> 6);
            ret += 0x80 | (v & 0x3f);
        }
    }

    return ret;
}

} // namespace

void MemoryCardExample::prepare()
{
    psyqo::GPU::Configuration config;
    config.set(psyqo::GPU::Resolution::W640)
        .set(psyqo::GPU::VideoMode::NTSC)
        .set(psyqo::GPU::ColorMode::C15BITS)
        .set(psyqo::GPU::Interlace::INTERLACED);
    gpu().initialize(config);
    m_card.prepare();
}

void MemoryCardExample::createScene()
{
    m_font.uploadSystemFont(gpu());
    m_input.initialize();
    pushScene(&scene);

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

    for (int i = 0; i < count; i++) {
        uint32_t outLen = 0;
        e = fs.readFileBlocking(
            app.gpu(), port, entries[i].name, g_readback, sizeof(g_readback), &outLen);
        if (e != MC::Error::OK) {
            printf("Failed to read file %i\n", i);
            return;
        }

        auto str = toUtf8(entries[i].name);
        printf("%d (%d blocks, datalen=%d): %s\n", i, entries[i].sizeInBlocks, outLen, str.c_str());
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
        gpu().waitChainIdle();
        gpu().uploadToVRAM(info.icon.clut, region);
        gpu().waitChainIdle();

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
            gpu().waitChainIdle();
            gpu().uploadToVRAM((const std::uint16_t*)info.icon.pixels[j], region);
            gpu().waitChainIdle();
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
}

void MemoryCardScene::frame()
{
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

    psyqo::PrimPieces::TPageLoc loc;

#if 1
    loc.setPageX(10);
    loc.setPageY(1);
        drawSprite(gpu(),
                16, 200,
                0, 0,
                255, 255,
                // save.texX + save.currFrame * 16 + 16, save.texY + 16,
                psyqo::Color{255, 255, 255},
                loc,
                psyqo::PrimPieces::ClutIndex({.x = 640+64, .y=3}));

    loc.setPageX(11);
    loc.setPageY(1);
        drawSprite(gpu(),
                16+255, 200,
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

    /* if (!m_started) {
        print(16, 16, yellow, "psyqo memory card self-test");
        print(16, 48, white, "WARNING: pressing X will FORMAT the");
        print(16, 64, white, "memory card in PORT 1, erasing it.");
        print(16, 96, white, "Each X press runs the next action.");
        print(16, 208, cyan, "Press X to begin.");
    } else {
        for (int i = 0; i < g_lineCount; i++) {
            const LogLine& line = g_lines[i];
            int16_t y = int16_t(6 + i * 12);
            if (line.isResult) {
                print(8, y, white, line.text);
                print(240, y, line.pass ? green : red, line.pass ? "PASS" : "FAIL");
            } else {
                print(8, y, gray, line.text);
            }
        }
        if (m_finished) {
            print(8, 224, green, "All actions complete.");
        } else {
            print(8, 224, cyan, "Press X for the next action.");
        }
    }

    // Rising-edge detect so a held button advances exactly one step.
    bool cross = app.m_input.isButtonPressed(
        psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Button::Cross);
    if (cross && !m_prevCross && !m_finished) {
        if (!m_started) {
            m_started = true;
            note("Memory card self-test (port 1):");
        }
        bool more = runStep(m_index);
        m_index++;
        if (!more || m_index >= kStepCount) m_finished = true;
    }
    m_prevCross = cross; */
}

int main()
{
    return app.run();
}
