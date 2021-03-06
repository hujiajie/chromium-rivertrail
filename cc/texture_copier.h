// Copyright 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef TextureCopier_h
#define TextureCopier_h

#include "base/basictypes.h"
#include "base/memory/scoped_ptr.h"
#include "cc/program_binding.h"
#include "cc/shader.h"
#include "IntSize.h"
#include "third_party/khronos/GLES2/gl2.h"

namespace WebKit {
class WebGraphicsContext3D;
}

namespace cc {

class TextureCopier {
public:
    struct Parameters {
        unsigned sourceTexture;
        unsigned destTexture;
        IntSize size;
    };
    // Copy the base level contents of |sourceTexture| to |destTexture|. Both texture objects
    // must be complete and have a base level of |size| dimensions. The color formats do not need
    // to match, but |destTexture| must have a renderable format.
    virtual void copyTexture(Parameters) = 0;
    virtual void flush() = 0;

    virtual ~TextureCopier() { }
};

class AcceleratedTextureCopier : public TextureCopier {
public:
    static scoped_ptr<AcceleratedTextureCopier> create(WebKit::WebGraphicsContext3D* context, bool usingBindUniforms)
    {
        return make_scoped_ptr(new AcceleratedTextureCopier(context, usingBindUniforms));
    }
    virtual ~AcceleratedTextureCopier();

    virtual void copyTexture(Parameters) OVERRIDE;
    virtual void flush() OVERRIDE;

protected:
    AcceleratedTextureCopier(WebKit::WebGraphicsContext3D*, bool usingBindUniforms);

private:
    typedef ProgramBinding<VertexShaderPosTexIdentity, FragmentShaderRGBATex> BlitProgram;

    WebKit::WebGraphicsContext3D* m_context;
    GLuint m_fbo;
    GLuint m_positionBuffer;
    scoped_ptr<BlitProgram> m_blitProgram;
    bool m_usingBindUniforms;

    DISALLOW_COPY_AND_ASSIGN(AcceleratedTextureCopier);
};

}

#endif
