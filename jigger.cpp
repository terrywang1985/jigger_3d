#define NOMINMAX // 防止 windows.h 定义 min 和 max 宏
#include <windows.h>
#include <GL/gl.h>
#include <GL/glu.h>
#include <vector>
#include <string>
#include <iostream>
#include <cmath>
#include <algorithm>
#include <map>

// TinyGLTF & stb
#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "tiny_gltf.h"

HDC hDC;
HGLRC hRC;
tinygltf::Model g_model;

// 用于计算模型边界和中心点
float g_minX = 0, g_maxX = 0, g_minY = 0, g_maxY = 0, g_minZ = 0, g_maxZ = 0;
float g_centerX = 0, g_centerY = 0, g_centerZ = 0;
float g_modelScale = 1.0f;

// 纹理缓存
std::map<int, GLuint> g_textureCache;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

void InitOpenGL(HWND hwnd) {
    PIXELFORMATDESCRIPTOR pfd = {
        sizeof(PIXELFORMATDESCRIPTOR), 1,
        PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
        PFD_TYPE_RGBA, 32,
        0,0,0,0,0,0,
        0,0,0,0,0,0,0,
        24, 8, 0,
        PFD_MAIN_PLANE, 0,0,0,0
    };

    hDC = GetDC(hwnd);
    int pf = ChoosePixelFormat(hDC, &pfd);
    SetPixelFormat(hDC, pf, &pfd);
    hRC = wglCreateContext(hDC);
    wglMakeCurrent(hDC, hRC);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // 启用面剔除 - 这很重要！
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);

    // 启用光照
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);

    // 设置平滑着色
    glShadeModel(GL_SMOOTH);

    // 启用法线归一化
    glEnable(GL_NORMALIZE);
}

bool LoadGLTFModel(const char* path, tinygltf::Model& model) {
    tinygltf::TinyGLTF loader;
    std::string err, warn;
    bool ret = loader.LoadBinaryFromFile(&model, &err, &warn, path);
    if (!warn.empty()) std::cout << "Warn: " << warn << std::endl;
    if (!err.empty()) std::cout << "Err: " << err << std::endl;

    // 输出模型信息用于调试
    if (ret) {
        std::cout << "Model loaded: " << model.meshes.size() << " meshes" << std::endl;
        for (size_t i = 0; i < model.meshes.size(); ++i) {
            std::cout << "Mesh " << i << ": " << model.meshes[i].primitives.size() << " primitives" << std::endl;
        }

        // 输出材质信息
        std::cout << "Materials: " << model.materials.size() << std::endl;
        for (size_t i = 0; i < model.materials.size(); ++i) {
            const auto& mat = model.materials[i];
            std::cout << "Material " << i << ": " << mat.name << std::endl;
            if (mat.pbrMetallicRoughness.baseColorTexture.index >= 0) {
                std::cout << "  Has base color texture: " << mat.pbrMetallicRoughness.baseColorTexture.index << std::endl;
            }
            if (mat.pbrMetallicRoughness.baseColorFactor.size() == 4) {
                std::cout << "  Base color factor: ["
                    << mat.pbrMetallicRoughness.baseColorFactor[0] << ", "
                    << mat.pbrMetallicRoughness.baseColorFactor[1] << ", "
                    << mat.pbrMetallicRoughness.baseColorFactor[2] << ", "
                    << mat.pbrMetallicRoughness.baseColorFactor[3] << "]" << std::endl;
            }
        }
    }

    return ret;
}

// 加载纹理
GLuint LoadTexture(const tinygltf::Model& model, int textureIndex) {
    if (textureIndex < 0 || textureIndex >= model.textures.size()) return 0;

    const auto& texture = model.textures[textureIndex];
    if (texture.source < 0 || texture.source >= model.images.size()) return 0;

    const auto& image = model.images[texture.source];

    GLuint texId;
    glGenTextures(1, &texId);
    glBindTexture(GL_TEXTURE_2D, texId);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    GLenum format = GL_RGBA;
    if (image.component == 3) {
        format = GL_RGB;
    }

    glTexImage2D(GL_TEXTURE_2D, 0, format, image.width, image.height, 0, format, GL_UNSIGNED_BYTE, &image.image[0]);

    return texId;
}

// 计算模型边界和缩放比例
void CalculateModelBounds(const tinygltf::Model& model) {
    g_minX = g_minY = g_minZ = FLT_MAX;
    g_maxX = g_maxY = g_maxZ = -FLT_MAX;

    for (const auto& mesh : model.meshes) {
        for (const auto& prim : mesh.primitives) {
            auto it = prim.attributes.find("POSITION");
            if (it == prim.attributes.end()) continue;

            const auto& accessor = model.accessors[it->second];
            const auto& view = model.bufferViews[accessor.bufferView];
            const auto& buffer = model.buffers[view.buffer];

            const float* positions = reinterpret_cast<const float*>(
                &buffer.data[view.byteOffset + accessor.byteOffset]);

            int stride = view.byteStride > 0 ? view.byteStride / sizeof(float) : 3;

            for (int i = 0; i < accessor.count; ++i) {
                const float* v = &positions[i * stride];
                g_minX = (std::min)(g_minX, v[0]);
                g_maxX = (std::max)(g_maxX, v[0]);
                g_minY = (std::min)(g_minY, v[1]);
                g_maxY = (std::max)(g_maxY, v[1]);
                g_minZ = (std::min)(g_minZ, v[2]);
                g_maxZ = (std::max)(g_maxZ, v[2]);
            }
        }
    }

    // 计算中心点
    g_centerX = (g_minX + g_maxX) / 2.0f;
    g_centerY = (g_minY + g_maxY) / 2.0f;
    g_centerZ = (g_minZ + g_maxZ) / 2.0f;

    // 计算缩放比例，使模型适合视图
    float sizeX = g_maxX - g_minX;
    float sizeY = g_maxY - g_minY;
    float sizeZ = g_maxZ - g_minZ;
    float maxSize = (std::max)((std::max)(sizeX, sizeY), sizeZ);

    if (maxSize > 0) {
        g_modelScale = 2.0f / maxSize;
    }

    std::cout << "Model bounds: X(" << g_minX << " to " << g_maxX << "), "
        << "Y(" << g_minY << " to " << g_maxY << "), "
        << "Z(" << g_minZ << " to " << g_maxZ << ")" << std::endl;
    std::cout << "Model center: (" << g_centerX << ", " << g_centerY << ", " << g_centerZ << ")" << std::endl;
    std::cout << "Model scale: " << g_modelScale << std::endl;
}

// 更安全的数据获取函数
template<typename T>
const T* GetTypedDataPointer(const tinygltf::Model& model, const std::string& attribute,
    const tinygltf::Primitive& prim, int* count = nullptr, int* stride = nullptr) {
    auto it = prim.attributes.find(attribute);
    if (it == prim.attributes.end()) return nullptr;

    const auto& accessor = model.accessors[it->second];
    const auto& view = model.bufferViews[accessor.bufferView];
    const auto& buffer = model.buffers[view.buffer];

    if (count) *count = accessor.count;
    if (stride) {
        // 使用bufferView的stride，如果没有则使用默认stride
        if (view.byteStride > 0) {
            *stride = view.byteStride;
        }
        else {
            int components = 1;
            if (accessor.type == TINYGLTF_TYPE_VEC2) components = 2;
            else if (accessor.type == TINYGLTF_TYPE_VEC3) components = 3;
            else if (accessor.type == TINYGLTF_TYPE_VEC4) components = 4;
            *stride = sizeof(T) * components;
        }
    }

    return reinterpret_cast<const T*>(&buffer.data[view.byteOffset + accessor.byteOffset]);
}

// 设置材质
void SetMaterial(const tinygltf::Model& model, int materialIndex) {
    if (materialIndex < 0 || materialIndex >= model.materials.size()) {
        // 使用默认材质
        GLfloat defaultAmbient[] = { 0.5f, 0.5f, 0.5f, 1.0f };
        GLfloat defaultDiffuse[] = { 0.8f, 0.8f, 0.8f, 1.0f };
        glMaterialfv(GL_FRONT, GL_AMBIENT, defaultAmbient);
        glMaterialfv(GL_FRONT, GL_DIFFUSE, defaultDiffuse);
        return;
    }

    const auto& material = model.materials[materialIndex];
    const auto& pbr = material.pbrMetallicRoughness;

    // 设置基础颜色
    if (pbr.baseColorFactor.size() == 4) {
        GLfloat baseColor[] = {
            static_cast<GLfloat>(pbr.baseColorFactor[0]),
            static_cast<GLfloat>(pbr.baseColorFactor[1]),
            static_cast<GLfloat>(pbr.baseColorFactor[2]),
            static_cast<GLfloat>(pbr.baseColorFactor[3])
        };
        glMaterialfv(GL_FRONT, GL_DIFFUSE, baseColor);

        // 简化处理：使用基础颜色作为环境光
        GLfloat ambientColor[] = {
            static_cast<GLfloat>(pbr.baseColorFactor[0] * 0.5),
            static_cast<GLfloat>(pbr.baseColorFactor[1] * 0.5),
            static_cast<GLfloat>(pbr.baseColorFactor[2] * 0.5),
            static_cast<GLfloat>(pbr.baseColorFactor[3])
        };
        glMaterialfv(GL_FRONT, GL_AMBIENT, ambientColor);
    }

    // 处理基础颜色纹理
    if (pbr.baseColorTexture.index >= 0) {
        int texIndex = pbr.baseColorTexture.index;
        if (g_textureCache.find(texIndex) == g_textureCache.end()) {
            g_textureCache[texIndex] = LoadTexture(model, texIndex);
        }

        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, g_textureCache[texIndex]);
    }
    else {
        glDisable(GL_TEXTURE_2D);
    }
}

// 修复后的DrawModel函数
void DrawModel(const tinygltf::Model& model) {
    if (model.meshes.empty()) return;

    // 设置光源
    GLfloat lightPos[] = { 5.0f, 5.0f, 5.0f, 1.0f };
    GLfloat lightAmbient[] = { 0.3f, 0.3f, 0.3f, 1.0f };
    GLfloat lightDiffuse[] = { 1.0f, 1.0f, 1.0f, 1.0f };
    GLfloat lightSpecular[] = { 1.0f, 1.0f, 1.0f, 1.0f };

    glLightfv(GL_LIGHT0, GL_POSITION, lightPos);
    glLightfv(GL_LIGHT0, GL_AMBIENT, lightAmbient);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, lightDiffuse);
    glLightfv(GL_LIGHT0, GL_SPECULAR, lightSpecular);

    for (size_t meshIndex = 0; meshIndex < model.meshes.size(); ++meshIndex) {
        const auto& mesh = model.meshes[meshIndex];

        for (size_t primIndex = 0; primIndex < mesh.primitives.size(); ++primIndex) {
            const auto& prim = mesh.primitives[primIndex];

            // 调试信息
            std::cout << "Drawing mesh " << meshIndex << ", primitive " << primIndex << std::endl;
            std::cout << "Primitive mode: " << prim.mode << std::endl;

            // 设置材质
            SetMaterial(model, prim.material);

            // 获取顶点位置
            int posCount = 0, posStride = 0;
            const float* positions = GetTypedDataPointer<float>(model, "POSITION", prim, &posCount, &posStride);
            if (!positions) {
                std::cout << "No positions found for primitive" << std::endl;
                continue;
            }

            // 获取法线
            int normCount = 0, normStride = 0;
            const float* normals = GetTypedDataPointer<float>(model, "NORMAL", prim, &normCount, &normStride);

            // 获取纹理坐标
            int texCount = 0, texStride = 0;
            const float* texCoords = GetTypedDataPointer<float>(model, "TEXCOORD_0", prim, &texCount, &texStride);

            std::cout << "Vertex count: " << posCount << std::endl;

            // 检查是否有索引
            if (prim.indices < 0) {
                std::cout << "Drawing without indices" << std::endl;
                // 没有索引，直接绘制顶点
                glBegin(GL_TRIANGLES);
                for (int i = 0; i < posCount; i++) {
                    if (normals && i < normCount) {
                        const float* n = reinterpret_cast<const float*>(
                            reinterpret_cast<const char*>(normals) + i * normStride);
                        glNormal3f(n[0], n[1], n[2]);
                    }

                    if (texCoords && i < texCount) {
                        const float* tc = reinterpret_cast<const float*>(
                            reinterpret_cast<const char*>(texCoords) + i * texStride);
                        glTexCoord2f(tc[0], tc[1]);
                    }

                    const float* v = reinterpret_cast<const float*>(
                        reinterpret_cast<const char*>(positions) + i * posStride);
                    glVertex3f(v[0], v[1], v[2]);
                }
                glEnd();
                continue;
            }

            // 有索引的情况
            const auto& idxAccessor = model.accessors[prim.indices];
            const auto& idxView = model.bufferViews[idxAccessor.bufferView];
            const auto& idxBuf = model.buffers[idxView.buffer];

            std::cout << "Index count: " << idxAccessor.count << std::endl;
            std::cout << "Index component type: " << idxAccessor.componentType << std::endl;

            // 处理不同的图元类型
            GLenum drawMode = GL_TRIANGLES;
            if (prim.mode == TINYGLTF_MODE_POINTS) drawMode = GL_POINTS;
            else if (prim.mode == TINYGLTF_MODE_LINE) drawMode = GL_LINES;
            else if (prim.mode == TINYGLTF_MODE_LINE_STRIP) drawMode = GL_LINE_STRIP;
            else if (prim.mode == TINYGLTF_MODE_TRIANGLE_STRIP) drawMode = GL_TRIANGLE_STRIP;
            else if (prim.mode == TINYGLTF_MODE_TRIANGLE_FAN) drawMode = GL_TRIANGLE_FAN;

            glBegin(drawMode);

            // 根据索引类型处理
            if (idxAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
                const unsigned short* indices = reinterpret_cast<const unsigned short*>(
                    &idxBuf.data[idxView.byteOffset + idxAccessor.byteOffset]);

                for (size_t i = 0; i < idxAccessor.count; i++) {
                    int idx = indices[i];

                    if (idx >= posCount) {
                        std::cout << "Index out of bounds: " << idx << " >= " << posCount << std::endl;
                        continue;
                    }

                    if (normals && idx < normCount) {
                        const float* n = reinterpret_cast<const float*>(
                            reinterpret_cast<const char*>(normals) + idx * normStride);
                        glNormal3f(n[0], n[1], n[2]);
                    }

                    if (texCoords && idx < texCount) {
                        const float* tc = reinterpret_cast<const float*>(
                            reinterpret_cast<const char*>(texCoords) + idx * texStride);
                        glTexCoord2f(tc[0], tc[1]);
                    }

                    const float* v = reinterpret_cast<const float*>(
                        reinterpret_cast<const char*>(positions) + idx * posStride);
                    glVertex3f(v[0], v[1], v[2]);
                }
            }
            else if (idxAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
                const unsigned int* indices = reinterpret_cast<const unsigned int*>(
                    &idxBuf.data[idxView.byteOffset + idxAccessor.byteOffset]);

                for (size_t i = 0; i < idxAccessor.count; i++) {
                    int idx = indices[i];

                    if (idx >= posCount) {
                        std::cout << "Index out of bounds: " << idx << " >= " << posCount << std::endl;
                        continue;
                    }

                    if (normals && idx < normCount) {
                        const float* n = reinterpret_cast<const float*>(
                            reinterpret_cast<const char*>(normals) + idx * normStride);
                        glNormal3f(n[0], n[1], n[2]);
                    }

                    if (texCoords && idx < texCount) {
                        const float* tc = reinterpret_cast<const float*>(
                            reinterpret_cast<const char*>(texCoords) + idx * texStride);
                        glTexCoord2f(tc[0], tc[1]);
                    }

                    const float* v = reinterpret_cast<const float*>(
                        reinterpret_cast<const char*>(positions) + idx * posStride);
                    glVertex3f(v[0], v[1], v[2]);
                }
            }
            else if (idxAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
                const unsigned char* indices = reinterpret_cast<const unsigned char*>(
                    &idxBuf.data[idxView.byteOffset + idxAccessor.byteOffset]);

                for (size_t i = 0; i < idxAccessor.count; i++) {
                    int idx = indices[i];

                    if (idx >= posCount) {
                        std::cout << "Index out of bounds: " << idx << " >= " << posCount << std::endl;
                        continue;
                    }

                    if (normals && idx < normCount) {
                        const float* n = reinterpret_cast<const float*>(
                            reinterpret_cast<const char*>(normals) + idx * normStride);
                        glNormal3f(n[0], n[1], n[2]);
                    }

                    if (texCoords && idx < texCount) {
                        const float* tc = reinterpret_cast<const float*>(
                            reinterpret_cast<const char*>(texCoords) + idx * texStride);
                        glTexCoord2f(tc[0], tc[1]);
                    }

                    const float* v = reinterpret_cast<const float*>(
                        reinterpret_cast<const char*>(positions) + idx * posStride);
                    glVertex3f(v[0], v[1], v[2]);
                }
            }

            glEnd();
            glDisable(GL_TEXTURE_2D);
        }
    }
}

void Render(float angle) {
    glClearColor(0, 0, 0, 0); // 黑色 → 透明
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(45.0, 1.0, 0.1, 100.0);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    // 设置相机
    gluLookAt(0, 0, 3,  // 相机位置
        0, 0, 0,  // 观察点
        0, 1, 0); // 上方向

    // 旋转模型
    glRotatef(angle, 0, 1, 0);

    // 缩放和居中模型
    glScalef(g_modelScale, g_modelScale, g_modelScale);
    glTranslatef(-g_centerX, -g_centerY, -g_centerZ);

    // 设置默认材质属性
    GLfloat matSpecular[] = { 1.0f, 1.0f, 1.0f, 1.0f };
    GLfloat matShininess[] = { 50.0f };

    glMaterialfv(GL_FRONT, GL_SPECULAR, matSpecular);
    glMaterialfv(GL_FRONT, GL_SHININESS, matShininess);

    DrawModel(g_model);

    SwapBuffers(hDC);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    WNDCLASS wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"DesktopPet";
    RegisterClass(&wc);

    HWND hwnd = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TRANSPARENT,
        L"DesktopPet", L"Desktop Pet",
        WS_POPUP, 200, 200, 400, 400,
        NULL, NULL, hInstance, NULL
    );

    // 设置黑色为透明
    SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), 0, LWA_COLORKEY);

    ShowWindow(hwnd, SW_SHOW);
    InitOpenGL(hwnd);

    if (!LoadGLTFModel("bongo.glb", g_model)) {
        MessageBoxA(NULL, "加载 glb 失败！", "错误", MB_OK);
        return 0;
    }

    // 计算模型边界和缩放比例
    CalculateModelBounds(g_model);

    MSG msg;
    float angle = 0.0f;
    while (true) {
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto cleanup;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        Render(angle);
        angle += 0.5f;
        Sleep(16);
    }

cleanup:
    // 清理纹理
    for (auto& pair : g_textureCache) {
        glDeleteTextures(1, &pair.second);
    }

    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(hRC);
    ReleaseDC(hwnd, hDC);
    return 0;
}