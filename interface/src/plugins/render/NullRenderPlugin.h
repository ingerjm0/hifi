//
//  NullRenderPlugin.h
//
//  Created by Bradley Austin Davis on 2014/04/13.
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
#pragma once

#include "RenderPlugin.h"

class NullRenderPlugin : public RenderPlugin {
public:
    static const QString NAME;

    virtual ~NullRenderPlugin() final {}
    virtual const QString & getName();

    virtual QSize getRecommendedFramebufferSize() const;
    virtual glm::ivec2 getCanvasSize() const;
    virtual bool hasFocus() const;
    virtual glm::ivec2 getRelativeMousePosition() const;
    virtual glm::ivec2 getTrueMousePosition() const;
    virtual PickRay computePickRay(const glm::vec2 & pos) const;
    virtual bool isMouseOnScreen();

};
