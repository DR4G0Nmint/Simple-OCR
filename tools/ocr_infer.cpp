// Minimal OCR inference tool using NeoML
// Build: cmake -S . -B build && cmake --build build -j$(nproc)
// Usage: ./build/tools/ocr_infer /path/to/model.ocrbin
// Purpose: Deliverable 2+3: load a trained .ocrbin model and run forward pass

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

#include <cstdio>
#include <cstdlib>

using namespace NeoML;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <model.ocrbin>\n", argv[0]);
        return 1;
    }

    std::string modelPath = argv[1];

    std::fprintf(stdout, "OCR inference tool\n  model: %s\n", modelPath.c_str());

    IMathEngine& mathEngine = GetDefaultCpuMathEngine();
    CRandom random(0x5eed);

    CDnn dnn(random, mathEngine);

    {
        CArchiveFile file(modelPath.c_str(), CArchive::SD_Loading);
        if (!file.IsOpen()) {
            std::fprintf(stderr, "Cannot open model %s\n", modelPath.c_str());
            return 1;
        }
        CArchive archive(&file, CArchive::SD_Loading);
        archive.Serialize(dnn);
    }
    std::fprintf(stdout, "Loaded model: %d layers\n", dnn.GetLayerCount());

    // Get input blob (layer named "data")
    CPtr<CDataLayer> dataLayer = CheckCast<CDataLayer>(dnn.GetLayer("data"));
    if (!dataLayer) {
        std::fprintf(stderr, "Model missing 'data' layer\n");
        return 1;
    }

    CPtr<CDnnBlob> inputBlob = dataLayer->GetBlob();

    // Fill input with deterministic pattern matching 32x128 gray normalization
    {
        float* inputData = nullptr;
        {
            CDnnBlobBuffer<float> buf(*inputBlob, TDnnBlobBufferAccess::Write);
            int w = 128, h = 32;
            int total = w * h;
            for (int i = 0; i < total; ++i) {
                // Pattern: checkerboard of normalized values
                int x = i % w;
                int y = i / w;
                float v = ((x * 37 + y * 17) % 255) / 127.5f - 1.0f;
                buf[i] = v;
            }
        }
    }

    dnn.RunOnce();

    // Get output (sink/softmax layer)
    CPtr<CSinkLayer> sink = CheckCast<CSinkLayer>(dnn.GetLayer("softmax0"));
    if (!sink) {
        sink = CheckCast<CSinkLayer>(dnn.GetLayer("softmax"));
    }
    if (!sink) {
        std::fprintf(stderr, "No softmax/sink layer found\n");
        // Print available layers
    }

    CDnnBlob* outBlob = sink->GetBlob();
    std::fprintf(stdout, "Output size: %d\n", outBlob->GetDataSize());

    // Print first few output values
    CDnnBlobBuffer<float> buf(*outBlob, TDnnBlobBufferAccess::Read);
    std::fprintf(stdout, "Output values (first 16): ");
    int outCount = outBlob->GetDataSize();
    int displayCount = std::min(outCount, 16);
    for (int i = 0; i < displayCount; ++i) std::fprintf(stdout, "%.4f ", buf[i]);
    std::fprintf(stdout, "\n");

    // Find max class index (greedy decode start)
    int maxIdx = 0;
    float maxVal = buf[0];
    for (int i = 1; i < outCount; ++i) if (buf[i] > maxVal) { maxVal = buf[i]; maxIdx = i; }
    std::fprintf(stdout, "max class index: %d (of %d classes)\n", maxIdx, outCount);

    std::fprintf(stdout, "Inference complete.\n");
    return 0;
}