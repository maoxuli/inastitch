﻿// Copyright (C) 2020 Inatech srl
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// inastitch, Inatech's stitcher
// Created on 28.05.2020
// by Vincent Jordan

#define USE_OPENGL_PBO 1
// Note: Enabling PBO requires OpenGL ES 3.0
// See "#define GLFW_INCLUDE_ES3" below.

// Local includes:
#include "version.h"
#include "inastitch/jpeg/include/Decoder.hpp"
#include "inastitch/jpeg/include/Encoder.hpp"
#include "inastitch/jpeg/include/MjpegParser.hpp"
#include "inastitch/jpeg/include/RtpJpegParser.hpp"
#include "inastitch/opengl/include/OpenGlHelper.hpp"
#include "inastitch/json/include/Matrix.hpp"

// Boost includes:
#include <boost/program_options.hpp>
namespace po = boost::program_options;
// for thread_pool
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/post.hpp>

// GLM includes:
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

// Glfw includes:
// Use OpenGL ES 2.x
//#define GLFW_INCLUDE_ES2
// Use OpenGL ES 3.x
#define GLFW_INCLUDE_ES3
#include <GLFW/glfw3.h>

// Std includes:
#include <limits>
#include <string>
#include <iostream>
#include <chrono>
#include <thread>
#include <fstream>

// Note: the vertex shader describes how vertices (i.e., the 3 coords of a triangle)
//       are transformed.
static const GLchar* vertexShaderSource = R""""(
#version 100
precision mediump float;

attribute vec2 position;
attribute vec2 texCoord;

varying vec2 texCoordVar;
uniform mat4 model;
uniform mat4 view;
uniform mat4 proj;

void main() {
   gl_Position = proj * view * model * vec4(position.x, position.y, 0.0f, 1.0f);
   texCoordVar = texCoord;
}
)"""";

// Note: the pixel shader describes how individual pixels (i.e., the texture) within
//       a triangle are transformed.
static const GLchar* fragmentShaderSource = R""""(
#version 100
precision mediump float;

varying vec2 texCoordVar;
uniform sampler2D texture1;
uniform mat3 warp;

void main() {
   vec3 dst = warp * vec3((texCoordVar.x+1.0), texCoordVar.y, 1.0f);
   gl_FragColor = texture2D(texture1, vec2((dst.x/dst.z), (dst.y/dst.z)) );
}
)"""";

// Rectangle for the image texture
static const GLfloat topRightX    = -0.320f, topRightY    =  0.240f;
static const GLfloat bottomRightX = -0.320f, bottomRightY = -0.240f;
static const GLfloat bottomLeftX  =  0.320f, bottomLeftY  = -0.240f;
static const GLfloat topLeftX     =  0.320f, topLeftY     =  0.240f;
static const GLfloat vertices[] = {
    // position (2D)            // texCoord
    topRightX,    topRightY,    0.0f, 0.0f,
    bottomRightX, bottomRightY, 0.0f, 1.0f,
    bottomLeftX,  bottomLeftY,  1.0f, 1.0f,

    bottomLeftX,  bottomLeftY,  1.0f, 1.0f,
    topLeftX,     topLeftY,     1.0f, 0.0f,
    topRightX,    topRightY,    0.0f, 0.0f
};
// Note: UV texture coordinates are "inverted" to flip texture image,
//       since OpenGL reads image "upside-down".

struct GenericInputStreamContext
{
    GenericInputStreamContext(uint32_t maxRgbaBufferSize)
        : rgbaBufferSize(maxRgbaBufferSize)
    {
        jpegDecoderPtr = new inastitch::jpeg::Decoder(maxRgbaBufferSize);
    }

    ~GenericInputStreamContext()
    {
        delete jpegDecoderPtr;
    }

    virtual bool getFrame(uint32_t index) = 0;

    void decodeJpeg()
    {
        rgbaBuffer = jpegDecoderPtr->decode(jpegBuffer, jpegBufferSize);
    }

    void decodeWhite()
    {
        for(uint32_t i=0; i<rgbaBufferSize; i++)
        {
            jpegDecoderPtr->rgbaBuffer()[i] = 0xFF;
        }
    }

    inastitch::jpeg::Decoder *jpegDecoderPtr = nullptr;

    unsigned char *jpegBuffer = nullptr;
    unsigned char *rgbaBuffer = nullptr;
    const uint32_t rgbaBufferSize;
    uint32_t jpegBufferSize;

    // absolute time since epoch (in us)
    uint64_t absTime = 0;
    // relative time compared to other frames stitched together (is us)
    uint64_t relTime = 0;
    // offset time since previous frame of the same stream, aka "frame time" (in us)
    uint64_t offTime = 0;

    uint64_t timeDelay = 0;
};

template<class FrameParser>
struct InputStreamContext : GenericInputStreamContext
{
    InputStreamContext(uint32_t maxRgbaBufferSize, std::string streamLocationString)
        : GenericInputStreamContext(maxRgbaBufferSize)
    {
        jpegParserPtr = new FrameParser(streamLocationString, maxRgbaBufferSize);
        // Note: assumes RGBA data is always larger than JPEG data
    }

    bool getFrame(uint32_t index)
    {
        // JPEG frame
        const auto [_jpegBuffer, _jpegBufferSize, _absTime] = jpegParserPtr->getFrame(index);
        jpegBuffer = _jpegBuffer;
        jpegBufferSize = _jpegBufferSize;
        absTime = _absTime;
        // TODO: update relTime and offTime

        return (jpegBufferSize != 0);
    }

    ~InputStreamContext()
    {
        delete jpegParserPtr;
    }

    FrameParser *jpegParserPtr = nullptr;
};

int main(int argc, char** argv)
{
    std::string inMatrixJsonFilename;
    std::string inFilename0, inFilename1, inFilename2;
    std::string inSocketPort0, inSocketPort1, inSocketPort2;
    uint16_t inStreamWidth, inStreamHeight;
    uint16_t inTpoolSize;
    uint16_t windowWidth, windowHeight;
    std::string outFilename;
    uint64_t maxDumpFrameCount;
    std::string frameDumpPath;
    uint64_t frameDumpOffsetId;
    std::string frameDumpOffsetTimeStr;
    uint64_t frameDumpOffsetTime;
    uint64_t maxDelay;
    bool isDumpFrameIdRelativeToOffset = false;
    bool isOverlayEnabled = false;
    bool isStatsEnabled = false;

    bool isFileInput = false;

    std::cout << "Inatech stitcher "
              << inastitch::version::GIT_COMMIT_TAG
              << " (" << inastitch::version::GIT_COMMIT_DATE << ")"
              << std::endl;

    // Command-line parameter parsing
    {
        po::options_description desc("Allowed options");
        desc.add_options()
            ("in-matrix", po::value<std::string>(&inMatrixJsonFilename),
             "Read matrix from JSON FILENAME")

            ("in-file0", po::value<std::string>(&inFilename0),
             "Read MJPEG from FILENAME for central texture (0)")
            ("in-file1", po::value<std::string>(&inFilename1),
             "Read MJPEG from FILENAME for left texture (1)")
            ("in-file2", po::value<std::string>(&inFilename2),
             "Read MJPEG from FILENAME for right texture (2)")

            ("in-port0", po::value<std::string>(&inSocketPort0),
             "Listen for RTP/JPEG on PORT for central texture (0)")
            ("in-port1", po::value<std::string>(&inSocketPort1),
             "Listen for RTP/JPEG on PORT for left texture (1)")
            ("in-port2", po::value<std::string>(&inSocketPort2),
             "Listen for RTP/JPEG on PORT for right texture (2)")

            ("in-width", po::value<uint16_t>(&inStreamWidth)->default_value(640),
             "Input stream WIDTH")
            ("in-height", po::value<uint16_t>(&inStreamHeight)->default_value(480),
             "Input stream HEIGHT")
            ("in-tpool-size", po::value<uint16_t>(&inTpoolSize)->default_value(3),
             "Thread pool SIZE for input stream decoding")

            ("out-width", po::value<uint16_t>(&windowWidth)->default_value(1920),
             "OpenGL rendering and output stream WIDTH")
            ("out-height", po::value<uint16_t>(&windowHeight)->default_value(480),
             "OpenGL rendering and output stream HEIGHT")
            ("out-file", po::value<std::string>(&outFilename),
             "Write output MJPEG to FILENAME")

            ("max-dump-frame", po::value<uint64_t>(&maxDumpFrameCount)->default_value(std::numeric_limits<uint64_t>::max()),
             "Maximum frame count")
            ("frame-dump-path", po::value<std::string>(&frameDumpPath)->default_value(""),
             "Dump frame to PATH")
            ("frame-dump-offset-id", po::value<uint64_t>(&frameDumpOffsetId)->default_value(0),
             "Dump frame starting at ID")
            ("frame-dump-offset-time", po::value<std::string>(&frameDumpOffsetTimeStr)->default_value("0"),
             "Dump frame starting at TIME (sysclk unix timestamp in us)")
            ("max-delay", po::value<uint64_t>(&maxDelay)->default_value(std::numeric_limits<uint64_t>::max()), // delay in us
             "Max delay")
            ("frame-dump-id-from-0", "Dump frame ID relative to offset (i.e., always starts at 0), rather start of stream")
            ("print-overlay", "Print text overlay on output frame")

            ("stats,s", "Print stats")
            ("help,h", "Show help")
        ;

        po::variables_map vm;
        po::store(po::parse_command_line(argc, argv, desc), vm);
        po::notify(vm);

        if(vm.count("help")) {
            std::cout << desc << std::endl;
            return 0;
        }

        if(vm.count("stats")) {
            isStatsEnabled = true;
        }

        if(vm.count("frame-dump-id-from-0")) {
            isDumpFrameIdRelativeToOffset = true;
        }

        if(vm.count("print-overlay")) {
            isOverlayEnabled = true;
        }

        frameDumpOffsetTime = std::strtoull(frameDumpOffsetTimeStr.c_str(), nullptr, 0);

        if( (vm.count("in-file0") || vm.count("in-file1") || vm.count("in-file2")) )
        {
            isFileInput = true;
        }

        // Should not mix ile and network input
        if( isFileInput &&
            (vm.count("in-port0") || vm.count("in-port1") || vm.count("in-port2"))   )
        {
            std::cout << "Cannot mix file and network stream inputs" << std::endl;
            return 0;
        }
    }

    std::cout << "Input stream threads: " << inTpoolSize << std::endl;
    boost::asio::thread_pool threadPoolOutStream(1);

    GLuint glShaderProgram, glVextexBufferObject;
    GLint glShaderPositionAttrib, glShaderTexCoordAttrib;
    GLint glShaderModelMatrixUni, glShaderViewMatrixUni, glShaderProjMatrixUni;
    GLint glShaderWarpMatrixUni;
    GLFWwindow* glWindow;

    // OpenGL initialization
    {
        glfwInit();
        glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
        glWindow = glfwCreateWindow(windowWidth, windowHeight, __FILE__, NULL, NULL);
        glfwMakeContextCurrent(glWindow);

        printf("GL_VERSION  : %s\n", glGetString(GL_VERSION) );
        printf("GL_RENDERER : %s\n", glGetString(GL_RENDERER) );

        glShaderProgram = inastitch::opengl::helper::getShaderProgram(vertexShaderSource, fragmentShaderSource);
        GL_CHECK( glShaderPositionAttrib = glGetAttribLocation(glShaderProgram, "position") );
        GL_CHECK( glShaderTexCoordAttrib = glGetAttribLocation(glShaderProgram, "texCoord") );

        GL_CHECK( glEnable(GL_DEPTH_TEST) );
        GL_CHECK( glClearColor(0.0f, 0.0f, 0.0f, 1.0f) );
        GL_CHECK( glViewport(0, 0, windowWidth, windowHeight) );

        GL_CHECK( glGenBuffers(1, &glVextexBufferObject) );
        GL_CHECK( glBindBuffer(GL_ARRAY_BUFFER, glVextexBufferObject) );
        GL_CHECK( glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW) );
        GL_CHECK( glVertexAttribPointer(glShaderPositionAttrib,  2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (GLvoid*)0) );
        GL_CHECK( glEnableVertexAttribArray(glShaderPositionAttrib) );
        GL_CHECK( glVertexAttribPointer(glShaderTexCoordAttrib, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (GLvoid*)(2 * sizeof(float))) );
        GL_CHECK( glEnableVertexAttribArray(glShaderTexCoordAttrib) );
        GL_CHECK( glBindBuffer(GL_ARRAY_BUFFER, 0) );
    }

    // video texture
    unsigned int textureWidth = inStreamWidth, textureHeight = inStreamHeight;
    GLuint texture0;
    GL_CHECK( glGenTextures(1, &texture0) );
    GL_CHECK( glBindTexture(GL_TEXTURE_2D, texture0) ); // bind
    GL_CHECK( glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT) );
    GL_CHECK( glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT) );
    GL_CHECK( glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST) );
    GL_CHECK( glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST) );
    GL_CHECK( glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, textureWidth, textureHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr) );
    GL_CHECK( glBindTexture(GL_TEXTURE_2D, 0) );        // unbind

    uint32_t overlayWidth = windowWidth/2, overlayHeight = windowHeight/2;
    inastitch::opengl::helper::Overlay overlayHelper(overlayWidth, overlayHeight);

    // overlay texture
    unsigned int textureOverlay;
    GL_CHECK( glGenTextures(1, &textureOverlay) );
    GL_CHECK( glBindTexture(GL_TEXTURE_2D, textureOverlay) );  // bind
    GL_CHECK( glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT) );
    GL_CHECK( glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT) );
    GL_CHECK( glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST) );
    GL_CHECK( glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST) );
    GL_CHECK( glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, overlayWidth, overlayHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr) );
    GL_CHECK( glBindTexture(GL_TEXTURE_2D, 0) );  // unbind

    const auto pixelSize = 4; // RGBA
    const auto pboBufferSize = windowWidth * windowHeight * pixelSize;
#if USE_OPENGL_PBO
    // PBO
    const auto pboCount = 2;
    unsigned int pboIds[pboCount];
    glGenBuffers(pboCount, pboIds);
    for(int i=0; i<pboCount; i++)
    {
         GL_CHECK( glBindBuffer(GL_PIXEL_PACK_BUFFER, pboIds[i]) );
         GL_CHECK( glBufferData(GL_PIXEL_PACK_BUFFER, pboBufferSize, nullptr, GL_STREAM_READ ) );
    }
#endif

    const glm::mat4 identMat4 = glm::mat4(1.0f);
    const glm::mat4 initialViewMat = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -2.0f));
    glm::mat4 modelMat[3] = {identMat4, identMat4, identMat4};
    glm::mat4 viewMat[3] =  {initialViewMat, initialViewMat, initialViewMat};
    glm::mat4 projMat = glm::perspective(glm::radians(45.0f), static_cast<float>(windowWidth)/windowHeight, 0.1f, 100.0f);

    const glm::mat3 identMat3 = glm::mat3(1.0f);
    glm::mat3 texWarpMat[3] = {identMat3, identMat3, identMat3};

    const auto outStreamMaxRgbBufferSize = windowWidth * windowHeight * 3;
    inastitch::jpeg::Encoder rtpJpegEncoder(outStreamMaxRgbBufferSize);
    // Note: JPEG data should be smaller than raw RGB data

    // read matrix settings from JSON file
    {
        const tao::json::value json = tao::json::from_file(inMatrixJsonFilename);

        using namespace inastitch::json;
        jsonToGlmMat4(json.at("texture0").as<std::vector<float>>("model"), modelMat[0]);
        jsonToGlmMat4(json.at("texture0").as<std::vector<float>>("view"), viewMat[0]);
        jsonToGlmMat3(json.at("texture0").as<std::vector<float>>("warp"), texWarpMat[0]);

        jsonToGlmMat4(json.at("texture1").as<std::vector<float>>("model"), modelMat[1]);
        jsonToGlmMat4(json.at("texture1").as<std::vector<float>>("view"), viewMat[1]);
        jsonToGlmMat3(json.at("texture1").as<std::vector<float>>("warp"), texWarpMat[1]);

        jsonToGlmMat4(json.at("texture2").as<std::vector<float>>("model"), modelMat[2]);
        jsonToGlmMat4(json.at("texture2").as<std::vector<float>>("view"), viewMat[2]);
        jsonToGlmMat3(json.at("texture2").as<std::vector<float>>("warp"), texWarpMat[2]);
    }

    const auto inStreamMaxRgbBufferSize = inStreamWidth * inStreamHeight * 4; // RGBA format

    std::unique_ptr<GenericInputStreamContext> inStreamContext0;
    std::unique_ptr<GenericInputStreamContext> inStreamContext1;
    std::unique_ptr<GenericInputStreamContext> inStreamContext2;
    if(isFileInput)
    {
        inStreamContext0 = std::make_unique<InputStreamContext<inastitch::jpeg::MjpegParser>>(inStreamMaxRgbBufferSize, inFilename0);
        inStreamContext1 = std::make_unique<InputStreamContext<inastitch::jpeg::MjpegParser>>(inStreamMaxRgbBufferSize, inFilename1);
        inStreamContext2 = std::make_unique<InputStreamContext<inastitch::jpeg::MjpegParser>>(inStreamMaxRgbBufferSize, inFilename2);
    }
    else
    {
        inStreamContext0 = std::make_unique<InputStreamContext<inastitch::jpeg::RtpJpegParser>>(inStreamMaxRgbBufferSize, inSocketPort0);
        inStreamContext1 = std::make_unique<InputStreamContext<inastitch::jpeg::RtpJpegParser>>(inStreamMaxRgbBufferSize, inSocketPort1);
        inStreamContext2 = std::make_unique<InputStreamContext<inastitch::jpeg::RtpJpegParser>>(inStreamMaxRgbBufferSize, inSocketPort2);
    }

    // parse first frames before entering the loop
    {
        inStreamContext0->getFrame(0);
        inStreamContext1->getFrame(0);
        inStreamContext2->getFrame(0);
    }

    // prepare output file
    auto outJpegFile = std::ofstream(outFilename, std::ios::binary);
    auto outPtsFile = std::ofstream(outFilename + ".pts");

    bool isFirstFrame = true;
    uint64_t frameCount = 0;
    uint64_t frameRelTime = 0;
    uint64_t lastFrameAbsTime = 0;
    uint64_t frameDumpCount = 0;
    std::cout << "DumpTimeOffset: " << frameDumpOffsetTime << std::endl;

    const auto renderTimeStart = std::chrono::high_resolution_clock::now();

    // This is the rendering loop
    while(!glfwWindowShouldClose(glWindow) && (frameDumpCount < maxDumpFrameCount))
    {
        const auto frameT1 = std::chrono::high_resolution_clock::now();

        uint64_t frameAbsTime = 0;
        {
            // min of 3 input frame timestamps
            frameAbsTime = std::min(inStreamContext0->absTime, std::min(inStreamContext1->absTime, inStreamContext2->absTime));
            inStreamContext0->timeDelay = inStreamContext0->absTime - frameAbsTime;
            inStreamContext1->timeDelay = inStreamContext1->absTime - frameAbsTime;
            inStreamContext2->timeDelay = inStreamContext2->absTime - frameAbsTime;

            bool eofContext0 = false, eofContext1 = false, eofContext2 = false;

            if(inStreamContext0->timeDelay < maxDelay)
            {
                eofContext0 = !inStreamContext0->getFrame(0);
            }
            if(inStreamContext1->timeDelay < maxDelay)
            {
                eofContext1 = !inStreamContext1->getFrame(0);
            }
            if(inStreamContext2->timeDelay < maxDelay)
            {
                eofContext2 = !inStreamContext2->getFrame(0);
            }

            if(eofContext0 && eofContext1 && eofContext2)
            {
                // No more frames to process,
                // break rendering loop
                //break;
            }
        }
        if(isFirstFrame)
        {
            lastFrameAbsTime = frameAbsTime;
        }
        const auto frameDiffTime = frameAbsTime - lastFrameAbsTime;
        frameRelTime = frameRelTime + frameDiffTime;

        const auto frameT2 = std::chrono::high_resolution_clock::now();
        // input JPEG decoding time
        
        const bool isFrameDumped = (frameCount >= frameDumpOffsetId) &&
                                   (frameAbsTime >= frameDumpOffsetTime);
        const auto frameDumpIdx = isDumpFrameIdRelativeToOffset ? frameDumpCount: frameCount;

        auto dumpJpegAndPtsAndTxt = [](const decltype(inStreamContext0) &inStreamCtx, const std::string &filename)
        {
            {
                auto jpegFile = std::fstream(filename, std::ios::out | std::ios::binary);
                jpegFile.write((char*)inStreamCtx->jpegBuffer, inStreamCtx->jpegBufferSize);
                jpegFile.close();
            }
            {
                auto ptsFile = std::fstream(filename + ".pts", std::ios::out);
                ptsFile << inStreamCtx->absTime << " " << inStreamCtx->relTime << " " << inStreamCtx->offTime << std::endl;
                ptsFile.close();
            }
        };

        boost::asio::thread_pool threadPoolInDecode(inTpoolSize);
        boost::asio::post(threadPoolInDecode,
            [&]()
            {
                if( (inStreamContext0->jpegBufferSize > 0) && (inStreamContext0->timeDelay < maxDelay) )
                {
                    if(isFrameDumped && !frameDumpPath.empty())
                    {
                        dumpJpegAndPtsAndTxt(inStreamContext0, frameDumpPath + std::to_string(frameDumpIdx) + "in0.jpg");
                    }
                    inStreamContext0->decodeJpeg();
                } else {
                    inStreamContext0->decodeWhite();
                }
            }
        );
        boost::asio::post(threadPoolInDecode,
            [&]()
            {
                if( (inStreamContext1->jpegBufferSize > 0) && (inStreamContext1->timeDelay < maxDelay) )
                {
                    if(isFrameDumped && !frameDumpPath.empty())
                    {
                        dumpJpegAndPtsAndTxt(inStreamContext1, frameDumpPath + std::to_string(frameDumpIdx) + "in1.jpg");
                    }
                    inStreamContext1->decodeJpeg();
                } else {
                    inStreamContext1->decodeWhite();
                }
            }
        );
        boost::asio::post(threadPoolInDecode,
            [&]()
            {
                if( (inStreamContext2->jpegBufferSize > 0) && (inStreamContext2->timeDelay < maxDelay) )
                {
                    if(isFrameDumped && !frameDumpPath.empty())
                    {
                        dumpJpegAndPtsAndTxt(inStreamContext2, frameDumpPath + std::to_string(frameDumpIdx) + "in2.jpg");
                    }
                    inStreamContext2->decodeJpeg();
                } else {
                    inStreamContext2->decodeWhite();
                }
            }
        );
        threadPoolInDecode.join();

        const auto frameT3 = std::chrono::high_resolution_clock::now();
        // input frame dump time

        uint8_t *bmpBuffer0 = inStreamContext0->rgbaBuffer;
        uint8_t *bmpBuffer1 = inStreamContext1->rgbaBuffer;
        uint8_t *bmpBuffer2 = inStreamContext2->rgbaBuffer;

        glfwPollEvents();
        GL_CHECK( glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT) );

        GL_CHECK( glUseProgram(glShaderProgram) );
        // vertex shader
        GL_CHECK( glShaderModelMatrixUni = glGetUniformLocation(glShaderProgram, "model") );
        GL_CHECK( glShaderViewMatrixUni = glGetUniformLocation(glShaderProgram, "view") );
        GL_CHECK( glShaderProjMatrixUni = glGetUniformLocation(glShaderProgram, "proj") );
        // pixel shader
        GL_CHECK( glShaderWarpMatrixUni = glGetUniformLocation(glShaderProgram, "warp") );
        const auto frameT4 = std::chrono::high_resolution_clock::now();
        // clear and prepare shader time

        GL_CHECK( glBindTexture(GL_TEXTURE_2D, texture0) );
        if(bmpBuffer0 != nullptr)
        {
            GL_CHECK( glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, textureWidth, textureHeight, GL_RGBA, GL_UNSIGNED_BYTE, bmpBuffer0) );
            GL_CHECK( glUniformMatrix4fv(glShaderModelMatrixUni, 1, GL_FALSE, glm::value_ptr(modelMat[0])) );
            GL_CHECK( glUniformMatrix4fv(glShaderViewMatrixUni, 1, GL_FALSE, glm::value_ptr(viewMat[0])) );
            GL_CHECK( glUniformMatrix4fv(glShaderProjMatrixUni, 1, GL_FALSE, glm::value_ptr(projMat)) );
            GL_CHECK( glUniformMatrix3fv(glShaderWarpMatrixUni, 1, GL_FALSE, glm::value_ptr(texWarpMat[0])) );
            GL_CHECK( glDrawArrays(GL_TRIANGLES, 0, 6) );
        }

        if(bmpBuffer1 != nullptr)
        {
            GL_CHECK( glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, textureWidth, textureHeight, GL_RGBA, GL_UNSIGNED_BYTE, bmpBuffer1) );
            GL_CHECK( glUniformMatrix4fv(glShaderModelMatrixUni, 1, GL_FALSE, glm::value_ptr(modelMat[1])) );
            GL_CHECK( glUniformMatrix4fv(glShaderViewMatrixUni, 1, GL_FALSE, glm::value_ptr(viewMat[1])) );
            GL_CHECK( glUniformMatrix4fv(glShaderProjMatrixUni, 1, GL_FALSE, glm::value_ptr(projMat)) );
            GL_CHECK( glUniformMatrix3fv(glShaderWarpMatrixUni, 1, GL_FALSE, glm::value_ptr(texWarpMat[1])) );
            GL_CHECK( glDrawArrays(GL_TRIANGLES, 0, 6) );
        }

        if(bmpBuffer2 != nullptr)
        {
            GL_CHECK( glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, textureWidth, textureHeight, GL_RGBA, GL_UNSIGNED_BYTE, bmpBuffer2) );
            GL_CHECK( glUniformMatrix4fv(glShaderModelMatrixUni, 1, GL_FALSE, glm::value_ptr(modelMat[2])) );
            GL_CHECK( glUniformMatrix4fv(glShaderViewMatrixUni, 1, GL_FALSE, glm::value_ptr(viewMat[2])) );
            GL_CHECK( glUniformMatrix4fv(glShaderProjMatrixUni, 1, GL_FALSE, glm::value_ptr(projMat)) );
            GL_CHECK( glUniformMatrix3fv(glShaderWarpMatrixUni, 1, GL_FALSE, glm::value_ptr(texWarpMat[2])) );
            GL_CHECK( glDrawArrays(GL_TRIANGLES, 0, 6) );
        }
        GL_CHECK( glBindTexture(GL_TEXTURE_2D, 0) ); // unbind
        const auto frameT5 = std::chrono::high_resolution_clock::now();
        // render video texture time

        if(isOverlayEnabled)
        {
            overlayHelper.clear();

            const auto frameTime = std::chrono::duration_cast<std::chrono::milliseconds>(frameT5-frameT4).count();

            {
                const auto baseX = 10;
                const auto baseY = 5;
                const auto stepY = 7;

                static const char header[] = "Inatech stitcher";
                overlayHelper.putString(baseX, baseY+stepY*0, header, sizeof(header));
                overlayHelper.putString(baseX, baseY+stepY*1, "FRAME ", 6);
                overlayHelper.putNumber(baseX+30, baseY+stepY*1, frameCount, 8);
            }

            overlayHelper.putString(10,    224, "CAM1=", 5);
            overlayHelper.putNumber(10+25, 224, inStreamContext1->timeDelay, 6);
            overlayHelper.putString(10+68, 224, "us", 2);

            overlayHelper.putString(330,    224, "CAM0=", 5);
            overlayHelper.putNumber(330+25, 224, inStreamContext0->timeDelay, 6);
            overlayHelper.putString(330+68, 224, "us", 2);

            overlayHelper.putString(650,    224, "CAM2=", 5);
            overlayHelper.putNumber(650+25, 224, inStreamContext2->timeDelay, 6);
            overlayHelper.putString(650+68, 224, "us", 2);

            auto overlayModelMat = identMat4;
            overlayModelMat[0][0] = 3.15f;
            overlayModelMat[1][1] = 4.2f;
            glBindTexture(GL_TEXTURE_2D, textureOverlay);
            GL_CHECK( glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, overlayWidth, overlayHeight, GL_RGBA, GL_UNSIGNED_BYTE, overlayHelper.rgbaBuffer()) );
            GL_CHECK( glUniformMatrix4fv(glShaderModelMatrixUni, 1, GL_FALSE, glm::value_ptr(overlayModelMat)) );
            GL_CHECK( glUniformMatrix4fv(glShaderViewMatrixUni, 1, GL_FALSE, glm::value_ptr(identMat4)) );
            GL_CHECK( glUniformMatrix4fv(glShaderProjMatrixUni, 1, GL_FALSE, glm::value_ptr(identMat4)) );
            GL_CHECK( glUniformMatrix3fv(glShaderWarpMatrixUni, 1, GL_FALSE, glm::value_ptr(identMat3)) );
            
            // for the overlay texture to be transparent, one needs to enable "blend function"
            GL_CHECK( glEnable(GL_BLEND) );
            GL_CHECK( glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA) );
            GL_CHECK( glDrawArrays(GL_TRIANGLES, 0, 6) );
            GL_CHECK( glDisable(GL_BLEND) );
        }
        const auto frameT6 = std::chrono::high_resolution_clock::now();
        // render overlay time

        std::chrono::high_resolution_clock::time_point frameT7, frameT8;
        {
            // TODO: such a big buffer on stack is not a good idea
            uint8_t framebuffer[pboBufferSize];
            
#if !USE_OPENGL_PBO
            // FBO
            GL_CHECK( glReadPixels(0, 0, windowWidth, windowHeight, GL_RGBA, GL_UNSIGNED_BYTE, framebuffer) );
            // this is synchronous and very slow because it forces the OpenGL pipeline to finish all rendering
#else
            static int r_idx = 0;
            int p_idx = 0;
            r_idx = (r_idx + 1) % pboCount;
            p_idx = (r_idx + 1 ) % pboCount;

            {
                GL_CHECK( glBindBuffer(GL_PIXEL_PACK_BUFFER, pboIds[r_idx]) );
                GL_CHECK( glReadPixels(0, 0, windowWidth, windowHeight, GL_RGBA, GL_UNSIGNED_BYTE, nullptr) );
                // this is supposed to be asynchronous
                GL_CHECK( glBindBuffer(GL_PIXEL_PACK_BUFFER, 0) );
            }
            
            frameT7 = std::chrono::high_resolution_clock::now();
            
            {
                GL_CHECK( glBindBuffer(GL_PIXEL_PACK_BUFFER, pboIds[p_idx]) );
                unsigned char* ptr = static_cast<unsigned char*>(glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, pboBufferSize, GL_MAP_READ_BIT));
                memcpy(framebuffer, ptr, pboBufferSize);
                GL_CHECK( glUnmapBuffer(GL_PIXEL_PACK_BUFFER) );
                GL_CHECK( glBindBuffer(GL_PIXEL_PACK_BUFFER, 0) );
            }
#endif
            frameT8 = std::chrono::high_resolution_clock::now();
            // read back pixel time

#if 0
            if(isFrameDumped )
            {
                boost::asio::post(threadPoolOutStream,
                   [&]
                   {
                        auto [ jpegData, jpegSize ] = rtpJpegEncoder.encode(framebuffer, windowWidth, windowHeight);

                        const std::string ptsStr = std::to_string(frameAbsTime) + " "
                                                   + std::to_string(frameRelTime) + " "
                                                   + std::to_string(frameDiffTime);

                        if(!outFilename.empty())
                        {
                            // append to output MJPEG
                            outJpegFile.write((char*)&jpegData[0], jpegSize);

                            // append to output PTS
                            outPtsFile << ptsStr << std::endl;
                        }

                        if(!frameDumpPath.empty())
                        {
                            auto jpegFile = std::fstream(frameDumpPath + std::to_string(frameDumpIdx) + "out.jpg", std::ios::out | std::ios::binary);
                            jpegFile.write((char*)&jpegData[0], jpegSize);
                            jpegFile.close();

                            auto ptsFile = std::fstream(frameDumpPath + std::to_string(frameDumpIdx) + "out.jpg" + ".pts", std::ios::out);
                            ptsFile << ptsStr;
                            ptsFile.close();
                        }
                    }
                );
            }
#endif

            frameCount++;
            if(isFrameDumped) frameDumpCount++;
            if(isFirstFrame) isFirstFrame = false;
            lastFrameAbsTime = frameAbsTime;
        }
        const auto frameT9 = std::chrono::high_resolution_clock::now();
        // dump output frame

        glfwSwapBuffers(glWindow);
        
        const auto frameT10 = std::chrono::high_resolution_clock::now();
        std::cout << "[" << frameCount << "," << frameDumpCount
                  << "] t0:" << inStreamContext0->timeDelay
                  << ", t1:" << inStreamContext1->timeDelay
                  << ", t2:" << inStreamContext2->timeDelay << std::endl;

        if(isStatsEnabled)
        std::cout << "inParse:" << std::chrono::duration_cast<std::chrono::microseconds>(frameT2-frameT1).count() << "us"
                  << ", inDump: " << std::chrono::duration_cast<std::chrono::microseconds>(frameT3-frameT2).count() << "us"
                  << ", init:" << std::chrono::duration_cast<std::chrono::microseconds>(frameT4-frameT3).count() << "us"
                  << ", vRend:" << std::chrono::duration_cast<std::chrono::microseconds>(frameT5-frameT4).count() << "us"
                  << ", oRend:" << std::chrono::duration_cast<std::chrono::microseconds>(frameT6-frameT5).count() << "us"
                  << ", readB1:" << std::chrono::duration_cast<std::chrono::microseconds>(frameT7-frameT6).count() << "us"
                  << ", readB2:" << std::chrono::duration_cast<std::chrono::microseconds>(frameT8-frameT7).count() << "us"
                  << ", outDump:" << std::chrono::duration_cast<std::chrono::microseconds>(frameT9-frameT8).count() << "us"
                  << ", total:" << std::chrono::duration_cast<std::chrono::microseconds>(frameT10-frameT1).count() << "us"
                  << std::endl;
    }

    const auto renderTimeEnd = std::chrono::high_resolution_clock::now();
    const auto renderTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(renderTimeEnd-renderTimeStart).count();

    if(frameCount == 0)
    {
        std::cout << "No frame rendered" << std::endl;
    }
    else
    {
        std::cout << frameCount << " frames rendered in "
                  << renderTimeMs << "ms"
                  << " (" << frameCount/(renderTimeMs/1000) << " fps)"
                  << std::endl;
    }

    outJpegFile.close();
    outPtsFile.close();

    threadPoolOutStream.join();

    GL_CHECK( glDeleteBuffers(1, &glVextexBufferObject) );
    glfwTerminate();

    return 0;
}
