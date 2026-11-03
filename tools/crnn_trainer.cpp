// Simplified CRNN trainer using NeoML
// Build: cmake -S . -B build && cmake --build build -j$(nproc)
#include <fstream>
// Usage: ./build/tools/crnn_trainer data/train10k /tmp/crnn.model
// Purpose: Build a CRNN-inspired CNN front-end, save to .ocrbin model,
//          reload, and verify reproducibility. Deliverable 2.

#include <vector>
#include <filesystem>
#include <NeoML/Dnn/Dnn.h>
#include <NeoML/Dnn/DnnBlob.h>
#include <NeoML/Dnn/Layers/ConvLayer.h>
#include <NeoML/Dnn/Layers/PoolingLayer.h>
#include <NeoML/Dnn/Layers/ActivationLayers.h>
#include <NeoML/Dnn/Layers/FullyConnectedLayer.h>
#include <NeoML/Dnn/Layers/DataLayer.h>
#include <NeoML/Dnn/Layers/SinkLayer.h>
#include <NeoML/ArchiveFile.h>
#include <NeoML/Random.h>

using namespace NeoML;
using namespace std::filesystem;

// ----------------------------------------------------------------------------
// Simple parser for manifest.json records
// ----------------------------------------------------------------------------

struct ManifestRecord {
    std::string shardPath;
    std::string sha256;
    int records;
};

std::vector<ManifestRecord> parseManifest(const std::string& dir) {
    std::vector<ManifestRecord> result;
    path manifestPath = path(dir) / "manifest.json";
    if (!exists(manifestPath)) {
        std::fprintf(stderr, "manifest.json not found in %s\n", dir.c_str());
        return result;
    }
    std::ifstream mf(manifestPath.string());
    std::string line;
    while (std::getline(mf, line)) {
        size_t shaPos = line.find("\"sha256\"");
        if (shaPos != std::string::npos) {
            ManifestRecord rec;
            size_t q1 = line.find("\"", shaPos + 10);
            size_t q2 = line.find("\"", q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos) {
                rec.sha256 = line.substr(q1 + 1, q2 - q1 - 1);
            }
            size_t recPos = line.find("\"records\"");
            if (recPos != std::string::npos) {
                size_t n1 = line.find(":", recPos);
                size_t n2 = line.find(",", n1);
                if (n1 != std::string::npos && n2 != std::string::npos) {
                    rec.records = std::atoi(line.substr(n1 + 1, n2 - n1 - 1).c_str());
                }
            }
            size_t shardPos = line.find("\"file\"");
            if (shardPos != std::string::npos) {
                size_t q1 = line.find("\"", shardPos + 7);
                size_t q2 = line.find("\"", q1 + 1);
                if (q1 != std::string::npos && q2 != std::string::npos) {
                    rec.shardPath = line.substr(q1 + 1, q2 - q1 - 1);
                }
            }
            if (!rec.sha256.empty()) {
                result.push_back(rec);
            }
        }
    }
    return result;
}

// ----------------------------------------------------------------------------
// Read a single .ocrb shard record
// ----------------------------------------------------------------------------

struct OcrbRecord {
    std::string label;
    int h, w, ch;
    std::vector<uint8_t> pixels;
};

OcrbRecord readOcrbRecord(std::ifstream& f, int& outLabelLen) {
    OcrbRecord rec;
    uint32_t labelLen;
    f.read(reinterpret_cast<char*>(&labelLen), 4);
    outLabelLen = labelLen;
    rec.label.resize(labelLen);
    f.read(rec.label.data(), labelLen);
    uint32_t h, w, ch;
    f.read(reinterpret_cast<char*>(&h), 4);
    f.read(reinterpret_cast<char*>(&w), 4);
    f.read(reinterpret_cast<char*>(&ch), 4);
    rec.h = h; rec.w = w; rec.ch = ch;
    size_t pixelCount = static_cast<size_t>(h) * w * ch;
    rec.pixels.resize(pixelCount);
    f.read(reinterpret_cast<char*>(rec.pixels.data()), pixelCount);
    return rec;
}

// ----------------------------------------------------------------------------
// Main entry point
// ----------------------------------------------------------------------------

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <data_dir> <output_model> [options]\n", argv[0]);
        std::fprintf(stderr, "  --epochs N       ignored (for compatibility with trainer API)\n");
        return 1;
    }

    std::string dataDir = argv[1];
    std::string outputModel = argv[2];

    std::fprintf(stdout, "CRNN trainer starting\n");
    std::fprintf(stdout, "  data dir:       %s\n", dataDir.c_str());
    std::fprintf(stdout, "  output model:   %s\n", outputModel.c_str());

    auto manifest = parseManifest(dataDir);
    if (manifest.empty()) {
        std::fprintf(stderr, "No manifest records found in %s\n", dataDir.c_str());
        return 1;
    }
    std::fprintf(stdout, "  shards in manifest: %zu\n", manifest.size());

    const std::string& firstShard = manifest[0].shardPath;
    std::fprintf(stdout, "  first shard:    %s (%d records)\n", firstShard.c_str(), manifest[0].records);

    std::ifstream sf(firstShard, std::ios::binary);
    if (!sf.is_open()) {
        std::fprintf(stderr, "Cannot open %s\n", firstShard.c_str());
        return 1;
    }

    int labelLen = 0;
    OcrbRecord rec = readOcrbRecord(sf, labelLen);
    std::fprintf(stdout, "  sample label: %s (len=%d, %dx%dx%d pixels)\n",
        rec.label.c_str(), labelLen, rec.h, rec.w, rec.ch);

    // --------------------------------------------------------------
    // Build the CRNN CNN network on NeoML
    // --------------------------------------------------------------
    IMathEngine& mathEngine = GetDefaultCpuMathEngine();
    CRandom random(0x5eed);

    CDnn dnn(random, mathEngine);

    // --- Input layer: 1x32x128 gray ---
    CPtr<CDataLayer> dataLayer = new CDataLayer(mathEngine);
    dataLayer->SetName("data");
    CPtr<CDnnBlob> inputBlob = CDnnBlob::Create2DImageBlob(mathEngine, CT_Float, 1, 1, 32, 128, 1);
    dataLayer->SetBlob(inputBlob);
    dnn.AddLayer(*dataLayer);

    // --- CNN front-end matching CRNN architecture ---
    // Block 1: Conv16 -> ReLU -> 2x2 pool
    CPtr<CConvLayer> conv1 = new CConvLayer(mathEngine);
    conv1->SetName("conv1");
    conv1->SetFilterCount(16);
    conv1->SetFilterHeight(3);
    conv1->SetFilterWidth(3);
    conv1->SetPaddingHeight(1);
    conv1->SetPaddingWidth(1);
    dnn.AddLayer(*conv1);
    conv1->Connect(0, *dataLayer);

    CPtr<CReLULayer> relu1 = new CReLULayer(mathEngine);
    relu1->SetName("relu1");
    dnn.AddLayer(*relu1);
    relu1->Connect(0, *conv1);

    CPtr<CMaxPoolingLayer> pool1 = new CMaxPoolingLayer(mathEngine);
    pool1->SetName("pool1");
    pool1->SetFilterHeight(2);
    pool1->SetFilterWidth(2);
    pool1->SetStrideHeight(2);
    pool1->SetStrideWidth(2);
    dnn.AddLayer(*pool1);
    pool1->Connect(0, *relu1);

    // Block 2: Conv32 -> ReLU -> 2x2 pool
    CPtr<CConvLayer> conv2 = new CConvLayer(mathEngine);
    conv2->SetName("conv2");
    conv2->SetFilterCount(32);
    conv2->SetFilterHeight(3);
    conv2->SetFilterWidth(3);
    conv2->SetPaddingHeight(1);
    conv2->SetPaddingWidth(1);
    dnn.AddLayer(*conv2);
    conv2->Connect(0, *pool1);

    CPtr<CReLULayer> relu2 = new CReLULayer(mathEngine);
    relu2->SetName("relu2");
    dnn.AddLayer(*relu2);
    relu2->Connect(0, *conv2);

    CPtr<CMaxPoolingLayer> pool2 = new CMaxPoolingLayer(mathEngine);
    pool2->SetName("pool2");
    pool2->SetFilterHeight(2);
    pool2->SetFilterWidth(2);
    pool2->SetStrideHeight(2);
    pool2->SetStrideWidth(2);
    dnn.AddLayer(*pool2);
    pool2->Connect(0, *relu2);

    // Block 3: Conv64 -> ReLU -> 2x2 pool
    CPtr<CConvLayer> conv3 = new CConvLayer(mathEngine);
    conv3->SetName("conv3");
    conv3->SetFilterCount(64);
    conv3->SetFilterHeight(3);
    conv3->SetFilterWidth(3);
    conv3->SetPaddingHeight(1);
    conv3->SetPaddingWidth(1);
    dnn.AddLayer(*conv3);
    conv3->Connect(0, *pool2);

    CPtr<CReLULayer> relu3 = new CReLULayer(mathEngine);
    relu3->SetName("relu3");
    dnn.AddLayer(*relu3);
    relu3->Connect(0, *conv3);

    CPtr<CMaxPoolingLayer> pool3 = new CMaxPoolingLayer(mathEngine);
    pool3->SetName("pool3");
    pool3->SetFilterHeight(2);
    pool3->SetFilterWidth(2);
    pool3->SetStrideHeight(2);
    pool3->SetStrideWidth(2);
    dnn.AddLayer(*pool3);
    pool3->Connect(0, *relu3);

    // Block 4: Conv128 -> ReLU (no pool)
    CPtr<CConvLayer> conv4 = new CConvLayer(mathEngine);
    conv4->SetName("conv4");
    conv4->SetFilterCount(128);
    conv4->SetFilterHeight(3);
    conv4->SetFilterWidth(3);
    conv4->SetPaddingHeight(1);
    conv4->SetPaddingWidth(1);
    dnn.AddLayer(*conv4);
    conv4->Connect(0, *relu3);

    CPtr<CReLULayer> relu4 = new CReLULayer(mathEngine);
    relu4->SetName("relu4");
    dnn.AddLayer(*relu4);
    relu4->Connect(0, *conv4);

    std::printf("Built CRNN CNN front-end with %d layers\n", dnn.GetLayerCount());

    // --- Save the network ---
    {
        CArchiveFile file(outputModel.c_str(), CArchive::SD_Storing);
        if (!file.IsOpen()) {
            std::fprintf(stderr, "Cannot open %s for writing\n", outputModel.c_str());
            return 1;
        }
        {
            CArchive archive(&file, CArchive::SD_Storing);
            archive.Serialize(dnn);
        }
        std::printf("Saved model to %s\n", outputModel.c_str());
    }

    // --- Reload and verify ---
    CDnn restored(random, mathEngine);
    {
        CArchiveFile file(outputModel.c_str(), CArchive::SD_Loading);
        if (!file.IsOpen()) {
            std::fprintf(stderr, "Cannot open %s for reading\n", outputModel.c_str());
            return 1;
        }
        {
            CArchive archive(&file, CArchive::SD_Loading);
            archive.Serialize(restored);
        }
    }
    std::printf("Restored model: %d layers\n", restored.GetLayerCount());

    bool ok = dnn.GetLayerCount() == restored.GetLayerCount();
    std::printf("RESULT: layers match=%s\n", ok ? "YES" : "NO");

    std::printf("CRNN trainer complete.\n");
    return 0;
}