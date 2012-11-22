// Copyright 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "config.h"

#include "cc/layer_tree_host_common.h"

#include "FloatQuad.h"
#include "IntRect.h"
#include "cc/layer.h"
#include "cc/layer_impl.h"
#include "cc/layer_iterator.h"
#include "cc/layer_sorter.h"
#include "cc/math_util.h"
#include "cc/render_surface.h"
#include "cc/render_surface_impl.h"
#include <algorithm>
#include <public/WebTransformationMatrix.h>

using WebKit::WebTransformationMatrix;

namespace cc {

ScrollAndScaleSet::ScrollAndScaleSet()
{
}

ScrollAndScaleSet::~ScrollAndScaleSet()
{
}

IntRect LayerTreeHostCommon::calculateVisibleRect(const IntRect& targetSurfaceRect, const IntRect& layerBoundRect, const WebTransformationMatrix& transform)
{
    // Is this layer fully contained within the target surface?
    IntRect layerInSurfaceSpace = MathUtil::mapClippedRect(transform, layerBoundRect);
    if (targetSurfaceRect.contains(layerInSurfaceSpace))
        return layerBoundRect;

    // If the layer doesn't fill up the entire surface, then find the part of
    // the surface rect where the layer could be visible. This avoids trying to
    // project surface rect points that are behind the projection point.
    IntRect minimalSurfaceRect = targetSurfaceRect;
    minimalSurfaceRect.intersect(layerInSurfaceSpace);

    // Project the corners of the target surface rect into the layer space.
    // This bounding rectangle may be larger than it needs to be (being
    // axis-aligned), but is a reasonable filter on the space to consider.
    // Non-invertible transforms will create an empty rect here.
    const WebTransformationMatrix surfaceToLayer = transform.inverse();
    IntRect layerRect = enclosingIntRect(MathUtil::projectClippedRect(surfaceToLayer, FloatRect(minimalSurfaceRect)));
    layerRect.intersect(layerBoundRect);
    return layerRect;
}

template<typename LayerType>
static inline bool layerIsInExisting3DRenderingContext(LayerType* layer)
{
    // According to current W3C spec on CSS transforms, a layer is part of an established
    // 3d rendering context if its parent has transform-style of preserves-3d.
    return layer->parent() && layer->parent()->preserves3D();
}

template<typename LayerType>
static bool layerIsRootOfNewRenderingContext(LayerType* layer)
{
    // According to current W3C spec on CSS transforms (Section 6.1), a layer is the
    // beginning of 3d rendering context if its parent does not have transform-style:
    // preserve-3d, but this layer itself does.
    if (layer->parent())
        return !layer->parent()->preserves3D() && layer->preserves3D();

    return layer->preserves3D();
}

template<typename LayerType>
static bool isLayerBackFaceVisible(LayerType* layer)
{
    // The current W3C spec on CSS transforms says that backface visibility should be
    // determined differently depending on whether the layer is in a "3d rendering
    // context" or not. For Chromium code, we can determine whether we are in a 3d
    // rendering context by checking if the parent preserves 3d.

    if (layerIsInExisting3DRenderingContext(layer))
        return layer->drawTransform().isBackFaceVisible();

    // In this case, either the layer establishes a new 3d rendering context, or is not in
    // a 3d rendering context at all.
    return layer->transform().isBackFaceVisible();
}

template<typename LayerType>
static bool isSurfaceBackFaceVisible(LayerType* layer, const WebTransformationMatrix& drawTransform)
{
    if (layerIsInExisting3DRenderingContext(layer))
        return drawTransform.isBackFaceVisible();

    if (layerIsRootOfNewRenderingContext(layer))
        return layer->transform().isBackFaceVisible();

    // If the renderSurface is not part of a new or existing rendering context, then the
    // layers that contribute to this surface will decide back-face visibility for themselves.
    return false;
}

template<typename LayerType>
static inline bool layerClipsSubtree(LayerType* layer)
{
    return layer->masksToBounds() || layer->maskLayer();
}

template<typename LayerType>
static IntRect calculateVisibleContentRect(LayerType* layer)
{
    DCHECK(layer->renderTarget());

    // Nothing is visible if the layer bounds are empty.
    if (!layer->drawsContent() || layer->contentBounds().isEmpty() || layer->drawableContentRect().isEmpty())
        return IntRect();

    IntRect targetSurfaceClipRect;

    // First, compute visible bounds in target surface space.
    if (layer->renderTarget()->renderSurface()->clipRect().isEmpty())
        targetSurfaceClipRect = layer->drawableContentRect();
    else {
        // In this case the target surface does clip layers that contribute to it. So, we
        // have convert the current surface's clipRect from its ancestor surface space to
        // the current surface space.
        targetSurfaceClipRect = enclosingIntRect(MathUtil::projectClippedRect(layer->renderTarget()->renderSurface()->drawTransform().inverse(), layer->renderTarget()->renderSurface()->clipRect()));
        targetSurfaceClipRect.intersect(layer->drawableContentRect());
    }

    if (targetSurfaceClipRect.isEmpty())
        return IntRect();

    return LayerTreeHostCommon::calculateVisibleRect(targetSurfaceClipRect, IntRect(IntPoint(), layer->contentBounds()), layer->drawTransform());
}

static bool isScaleOrTranslation(const WebTransformationMatrix& m)
{
    return !m.m12() && !m.m13() && !m.m14()
           && !m.m21() && !m.m23() && !m.m24()
           && !m.m31() && !m.m32() && !m.m43()
           && m.m44();
}

static inline bool transformToParentIsKnown(LayerImpl*)
{
    return true;
}

static inline bool transformToParentIsKnown(Layer* layer)
{
    return !layer->transformIsAnimating();
}

static inline bool transformToScreenIsKnown(LayerImpl*)
{
    return true;
}

static inline bool transformToScreenIsKnown(Layer* layer)
{
    return !layer->screenSpaceTransformIsAnimating();
}

template<typename LayerType>
static bool layerShouldBeSkipped(LayerType* layer)
{
    // Layers can be skipped if any of these conditions are met.
    //   - does not draw content.
    //   - is transparent
    //   - has empty bounds
    //   - the layer is not double-sided, but its back face is visible.
    //
    // Some additional conditions need to be computed at a later point after the recursion is finished.
    //   - the intersection of render surface content and layer clipRect is empty
    //   - the visibleContentRect is empty
    //
    // Note, if the layer should not have been drawn due to being fully transparent,
    // we would have skipped the entire subtree and never made it into this function,
    // so it is safe to omit this check here.

    if (!layer->drawsContent() || layer->bounds().isEmpty())
        return true;

    LayerType* backfaceTestLayer = layer;
    if (layer->useParentBackfaceVisibility()) {
        DCHECK(layer->parent());
        DCHECK(!layer->parent()->useParentBackfaceVisibility());
        backfaceTestLayer = layer->parent();
    }

    // The layer should not be drawn if (1) it is not double-sided and (2) the back of the layer is known to be facing the screen.
    if (!backfaceTestLayer->doubleSided() && transformToScreenIsKnown(backfaceTestLayer) && isLayerBackFaceVisible(backfaceTestLayer))
        return true;

    return false;
}

static inline bool subtreeShouldBeSkipped(LayerImpl* layer)
{
    // The opacity of a layer always applies to its children (either implicitly
    // via a render surface or explicitly if the parent preserves 3D), so the
    // entire subtree can be skipped if this layer is fully transparent.
    return !layer->opacity();
}

static inline bool subtreeShouldBeSkipped(Layer* layer)
{
    // If the opacity is being animated then the opacity on the main thread is unreliable
    // (since the impl thread may be using a different opacity), so it should not be trusted.
    // In particular, it should not cause the subtree to be skipped.
    return !layer->opacity() && !layer->opacityIsAnimating();
}

template<typename LayerType>
static bool subtreeShouldRenderToSeparateSurface(LayerType* layer, bool axisAlignedWithRespectToParent)
{
    // The root layer has a special render surface that is set up externally, so
    // it shouldn't be treated as a surface in this code.
    if (!layer->parent())
        return false;

    // Cache this value, because otherwise it walks the entire subtree several times.
    bool descendantDrawsContent = layer->descendantDrawsContent();

    //
    // A layer and its descendants should render onto a new RenderSurfaceImpl if any of these rules hold:
    //

    // If we force it.
    if (layer->forceRenderSurface())
        return true;

    // If the layer uses a mask.
    if (layer->maskLayer())
        return true;

    // If the layer has a reflection.
    if (layer->replicaLayer())
        return true;

    // If the layer uses a CSS filter.
    if (!layer->filters().isEmpty() || !layer->backgroundFilters().isEmpty() || layer->filter())
        return true;

    // If the layer flattens its subtree (i.e. the layer doesn't preserve-3d), but it is
    // treated as a 3D object by its parent (i.e. parent does preserve-3d).
    if (layerIsInExisting3DRenderingContext(layer) && !layer->preserves3D() && descendantDrawsContent)
        return true;

    // If the layer clips its descendants but it is not axis-aligned with respect to its parent.
    if (layerClipsSubtree(layer) && !axisAlignedWithRespectToParent && descendantDrawsContent)
        return true;

    // If the layer has opacity != 1 and does not have a preserves-3d transform style.
    if (layer->opacity() != 1 && !layer->preserves3D() && descendantDrawsContent)
        return true;

    return false;
}

WebTransformationMatrix computeScrollCompensationForThisLayer(LayerImpl* scrollingLayer, const WebTransformationMatrix& parentMatrix)
{
    // For every layer that has non-zero scrollDelta, we have to compute a transform that can undo the
    // scrollDelta translation. In particular, we want this matrix to premultiply a fixed-position layer's
    // parentMatrix, so we design this transform in three steps as follows. The steps described here apply
    // from right-to-left, so Step 1 would be the right-most matrix:
    //
    //     Step 1. transform from target surface space to the exact space where scrollDelta is actually applied.
    //           -- this is inverse of the matrix in step 3
    //     Step 2. undo the scrollDelta
    //           -- this is just a translation by scrollDelta.
    //     Step 3. transform back to target surface space.
    //           -- this transform is the "partialLayerOriginTransform" = (parentMatrix * scale(layer->pageScaleDelta()));
    //
    // These steps create a matrix that both start and end in targetSurfaceSpace. So this matrix can
    // pre-multiply any fixed-position layer's drawTransform to undo the scrollDeltas -- as long as
    // that fixed position layer is fixed onto the same renderTarget as this scrollingLayer.
    //

    WebTransformationMatrix partialLayerOriginTransform = parentMatrix;
    partialLayerOriginTransform.multiply(scrollingLayer->implTransform());

    WebTransformationMatrix scrollCompensationForThisLayer = partialLayerOriginTransform; // Step 3
    scrollCompensationForThisLayer.translate(scrollingLayer->scrollDelta().width(), scrollingLayer->scrollDelta().height()); // Step 2
    scrollCompensationForThisLayer.multiply(partialLayerOriginTransform.inverse()); // Step 1
    return scrollCompensationForThisLayer;
}

WebTransformationMatrix computeScrollCompensationMatrixForChildren(Layer* currentLayer, const WebTransformationMatrix& currentParentMatrix, const WebTransformationMatrix& currentScrollCompensation)
{
    // The main thread (i.e. Layer) does not need to worry about scroll compensation.
    // So we can just return an identity matrix here.
    return WebTransformationMatrix();
}

WebTransformationMatrix computeScrollCompensationMatrixForChildren(LayerImpl* layer, const WebTransformationMatrix& parentMatrix, const WebTransformationMatrix& currentScrollCompensationMatrix)
{
    // "Total scroll compensation" is the transform needed to cancel out all scrollDelta translations that
    // occurred since the nearest container layer, even if there are renderSurfaces in-between.
    //
    // There are some edge cases to be aware of, that are not explicit in the code:
    //  - A layer that is both a fixed-position and container should not be its own container, instead, that means
    //    it is fixed to an ancestor, and is a container for any fixed-position descendants.
    //  - A layer that is a fixed-position container and has a renderSurface should behave the same as a container
    //    without a renderSurface, the renderSurface is irrelevant in that case.
    //  - A layer that does not have an explicit container is simply fixed to the viewport
    //    (i.e. the root renderSurface, and it would still compensate for root layer's scrollDelta).
    //  - If the fixed-position layer has its own renderSurface, then the renderSurface is
    //    the one who gets fixed.
    //
    // This function needs to be called AFTER layers create their own renderSurfaces.
    //

    // Avoid the overheads (including stack allocation and matrix initialization/copy) if we know that the scroll compensation doesn't need to be reset or adjusted.
    if (!layer->isContainerForFixedPositionLayers() && layer->scrollDelta().isZero() && !layer->renderSurface())
        return currentScrollCompensationMatrix;

    // Start as identity matrix.
    WebTransformationMatrix nextScrollCompensationMatrix;

    // If this layer is not a container, then it inherits the existing scroll compensations.
    if (!layer->isContainerForFixedPositionLayers())
        nextScrollCompensationMatrix = currentScrollCompensationMatrix;

    // If the current layer has a non-zero scrollDelta, then we should compute its local scrollCompensation
    // and accumulate it to the nextScrollCompensationMatrix.
    if (!layer->scrollDelta().isZero()) {
        WebTransformationMatrix scrollCompensationForThisLayer = computeScrollCompensationForThisLayer(layer, parentMatrix);
        nextScrollCompensationMatrix.multiply(scrollCompensationForThisLayer);
    }

    // If the layer created its own renderSurface, we have to adjust nextScrollCompensationMatrix.
    // The adjustment allows us to continue using the scrollCompensation on the next surface.
    //  Step 1 (right-most in the math): transform from the new surface to the original ancestor surface
    //  Step 2: apply the scroll compensation
    //  Step 3: transform back to the new surface.
    if (layer->renderSurface() && !nextScrollCompensationMatrix.isIdentity())
        nextScrollCompensationMatrix = layer->renderSurface()->drawTransform().inverse() * nextScrollCompensationMatrix * layer->renderSurface()->drawTransform();

    return nextScrollCompensationMatrix;
}

// There is no contentsScale on impl thread.
static inline void updateLayerContentsScale(LayerImpl*, const WebTransformationMatrix&, float, float) { }

static inline void updateLayerContentsScale(Layer* layer, const WebTransformationMatrix& combinedTransform, float deviceScaleFactor, float pageScaleFactor)
{
    float rasterScale = layer->rasterScale();
    if (!rasterScale) {
        rasterScale = 1;

        if (layer->automaticallyComputeRasterScale()) {
            FloatPoint transformScale = MathUtil::computeTransform2dScaleComponents(combinedTransform);
            float combinedScale = std::max(transformScale.x(), transformScale.y());
            rasterScale = combinedScale / deviceScaleFactor;
            if (!layer->boundsContainPageScale())
                rasterScale /= pageScaleFactor;
            layer->setRasterScale(rasterScale);
        }
    }

    float contentsScale = rasterScale * deviceScaleFactor;
    if (!layer->boundsContainPageScale())
        contentsScale *= pageScaleFactor;
    layer->setContentsScale(contentsScale);

    Layer* maskLayer = layer->maskLayer();
    if (maskLayer)
        maskLayer->setContentsScale(contentsScale);

    Layer* replicaMaskLayer = layer->replicaLayer() ? layer->replicaLayer()->maskLayer() : 0;
    if (replicaMaskLayer)
        replicaMaskLayer->setContentsScale(contentsScale);
}

// Should be called just before the recursive calculateDrawTransformsInternal().
template<typename LayerType, typename LayerList>
void setupRootLayerAndSurfaceForRecursion(LayerType* rootLayer, LayerList& renderSurfaceLayerList, const IntSize& deviceViewportSize)
{
    if (!rootLayer->renderSurface())
        rootLayer->createRenderSurface();

    rootLayer->renderSurface()->setContentRect(IntRect(IntPoint::zero(), deviceViewportSize));
    rootLayer->renderSurface()->clearLayerLists();

    DCHECK(renderSurfaceLayerList.empty());
    renderSurfaceLayerList.push_back(rootLayer);
}

// Recursively walks the layer tree starting at the given node and computes all the
// necessary transformations, clipRects, render surfaces, etc.
template<typename LayerType, typename LayerList, typename RenderSurfaceType, typename LayerSorter>
static void calculateDrawTransformsInternal(LayerType* layer, LayerType* rootLayer, const WebTransformationMatrix& parentMatrix,
    const WebTransformationMatrix& fullHierarchyMatrix, const WebTransformationMatrix& currentScrollCompensationMatrix,
    const IntRect& clipRectFromAncestor, bool ancestorClipsSubtree,
    RenderSurfaceType* nearestAncestorThatMovesPixels, LayerList& renderSurfaceLayerList, LayerList& layerList,
    LayerSorter* layerSorter, int maxTextureSize, float deviceScaleFactor, float pageScaleFactor, IntRect& drawableContentRectOfSubtree)
{
    // This function computes the new matrix transformations recursively for this
    // layer and all its descendants. It also computes the appropriate render surfaces.
    // Some important points to remember:
    //
    // 0. Here, transforms are notated in Matrix x Vector order, and in words we describe what
    //    the transform does from left to right.
    //
    // 1. In our terminology, the "layer origin" refers to the top-left corner of a layer, and the
    //    positive Y-axis points downwards. This interpretation is valid because the orthographic
    //    projection applied at draw time flips the Y axis appropriately.
    //
    // 2. The anchor point, when given as a FloatPoint object, is specified in "unit layer space",
    //    where the bounds of the layer map to [0, 1]. However, as a WebTransformationMatrix object,
    //    the transform to the anchor point is specified in "layer space", where the bounds
    //    of the layer map to [bounds.width(), bounds.height()].
    //
    // 3. Definition of various transforms used:
    //        M[parent] is the parent matrix, with respect to the nearest render surface, passed down recursively.
    //        M[root] is the full hierarchy, with respect to the root, passed down recursively.
    //        Tr[origin] is the translation matrix from the parent's origin to this layer's origin.
    //        Tr[origin2anchor] is the translation from the layer's origin to its anchor point
    //        Tr[origin2center] is the translation from the layer's origin to its center
    //        M[layer] is the layer's matrix (applied at the anchor point)
    //        M[sublayer] is the layer's sublayer transform (applied at the layer's center)
    //        S[layer2content] is the ratio of a layer's contentBounds() to its bounds().
    //
    //    Some composite transforms can help in understanding the sequence of transforms:
    //        compositeLayerTransform = Tr[origin2anchor] * M[layer] * Tr[origin2anchor].inverse()
    //        compositeSublayerTransform = Tr[origin2center] * M[sublayer] * Tr[origin2center].inverse()
    //
    //    In words, the layer transform is applied about the anchor point, and the sublayer transform is
    //    applied about the center of the layer.
    //
    // 4. When a layer (or render surface) is drawn, it is drawn into a "target render surface". Therefore the draw
    //    transform does not necessarily transform from screen space to local layer space. Instead, the draw transform
    //    is the transform between the "target render surface space" and local layer space. Note that render surfaces,
    //    except for the root, also draw themselves into a different target render surface, and so their draw
    //    transform and origin transforms are also described with respect to the target.
    //
    // Using these definitions, then:
    //
    // The draw transform for the layer is:
    //        M[draw] = M[parent] * Tr[origin] * compositeLayerTransform * S[layer2content]
    //                = M[parent] * Tr[layer->position() + anchor] * M[layer] * Tr[anchor2origin] * S[layer2content]
    //
    //        Interpreting the math left-to-right, this transforms from the layer's render surface to the origin of the layer in content space.
    //
    // The screen space transform is:
    //        M[screenspace] = M[root] * Tr[origin] * compositeLayerTransform * S[layer2content]
    //                       = M[root] * Tr[layer->position() + anchor] * M[layer] * Tr[anchor2origin] * S[layer2content]
    //
    //        Interpreting the math left-to-right, this transforms from the root render surface's content space to the local layer's origin in layer space.
    //
    // The transform hierarchy that is passed on to children (i.e. the child's parentMatrix) is:
    //        M[parent]_for_child = M[parent] * Tr[origin] * compositeLayerTransform * compositeSublayerTransform
    //                            = M[parent] * Tr[layer->position() + anchor] * M[layer] * Tr[anchor2origin] * compositeSublayerTransform
    //
    //        and a similar matrix for the full hierarchy with respect to the root.
    //
    // Finally, note that the final matrix used by the shader for the layer is P * M[draw] * S . This final product
    // is computed in drawTexturedQuad(), where:
    //        P is the projection matrix
    //        S is the scale adjustment (to scale up a canonical quad to the layer's size)
    //
    // When a render surface has a replica layer, that layer's transform is used to draw a second copy of the surface.
    // Transforms named here are relative to the surface, unless they specify they are relative to the replica layer.
    //
    // We will denote a scale by device scale S[deviceScale]
    //
    // The render surface draw transform to its target surface origin is:
    //        M[surfaceDraw] = M[owningLayer->Draw]
    //
    // The render surface origin transform to its the root (screen space) origin is:
    //        M[surface2root] =  M[owningLayer->screenspace] * S[deviceScale].inverse()
    //
    // The replica draw transform to its target surface origin is:
    //        M[replicaDraw] = S[deviceScale] * M[surfaceDraw] * Tr[replica->position() + replica->anchor()] * Tr[replica] * Tr[origin2anchor].inverse() * S[contentsScale].inverse()
    //
    // The replica draw transform to the root (screen space) origin is:
    //        M[replica2root] = M[surface2root] * Tr[replica->position()] * Tr[replica] * Tr[origin2anchor].inverse()
    //

    // If we early-exit anywhere in this function, the drawableContentRect of this subtree should be considered empty.
    drawableContentRectOfSubtree = IntRect();

    if (subtreeShouldBeSkipped(layer))
        return;

    IntRect clipRectForSubtree;
    bool subtreeShouldBeClipped = false;
    
    float drawOpacity = layer->opacity();
    bool drawOpacityIsAnimating = layer->opacityIsAnimating();
    if (layer->parent() && layer->parent()->preserves3D()) {
        drawOpacity *= layer->parent()->drawOpacity();
        drawOpacityIsAnimating |= layer->parent()->drawOpacityIsAnimating();
    }

    IntSize bounds = layer->bounds();
    FloatPoint anchorPoint = layer->anchorPoint();
    FloatPoint position = layer->position() - layer->scrollDelta();

    WebTransformationMatrix layerLocalTransform;
    // LT = Tr[origin] * Tr[origin2anchor]
    layerLocalTransform.translate3d(position.x() + anchorPoint.x() * bounds.width(), position.y() + anchorPoint.y() * bounds.height(), layer->anchorPointZ());
    // LT = Tr[origin] * Tr[origin2anchor] * M[layer]
    layerLocalTransform.multiply(layer->transform());
    // LT = Tr[origin] * Tr[origin2anchor] * M[layer] * Tr[anchor2origin]
    layerLocalTransform.translate3d(-anchorPoint.x() * bounds.width(), -anchorPoint.y() * bounds.height(), -layer->anchorPointZ());

    WebTransformationMatrix combinedTransform = parentMatrix;
    combinedTransform.multiply(layerLocalTransform);

    // The layer's contentsSize is determined from the combinedTransform, which then informs the
    // layer's drawTransform.
    updateLayerContentsScale(layer, combinedTransform, deviceScaleFactor, pageScaleFactor);

    // If there is a tranformation from the impl thread then it should be at the
    // start of the combinedTransform, but we don't want it to affect the contentsScale.
    combinedTransform = layer->implTransform() * combinedTransform;

    if (layer->fixedToContainerLayer()) {
        // Special case: this layer is a composited fixed-position layer; we need to
        // explicitly compensate for all ancestors' nonzero scrollDeltas to keep this layer
        // fixed correctly.
        combinedTransform = currentScrollCompensationMatrix * combinedTransform;
    }

    // The drawTransform that gets computed below is effectively the layer's drawTransform, unless
    // the layer itself creates a renderSurface. In that case, the renderSurface re-parents the transforms.
    WebTransformationMatrix drawTransform = combinedTransform;
    if (!layer->contentBounds().isEmpty() && !layer->bounds().isEmpty()) {
        // M[draw] = M[parent] * LT * S[layer2content]
        drawTransform.scaleNonUniform(layer->bounds().width() / static_cast<double>(layer->contentBounds().width()),
                                      layer->bounds().height() / static_cast<double>(layer->contentBounds().height()));
    }

    // layerScreenSpaceTransform represents the transform between root layer's "screen space" and local content space.
    WebTransformationMatrix layerScreenSpaceTransform = fullHierarchyMatrix;
    if (!layer->preserves3D())
        MathUtil::flattenTransformTo2d(layerScreenSpaceTransform);
    layerScreenSpaceTransform.multiply(drawTransform);
    layer->setScreenSpaceTransform(layerScreenSpaceTransform);

    bool animatingTransformToTarget = layer->transformIsAnimating();
    bool animatingTransformToScreen = animatingTransformToTarget;
    if (layer->parent()) {
        animatingTransformToTarget |= layer->parent()->drawTransformIsAnimating();
        animatingTransformToScreen |= layer->parent()->screenSpaceTransformIsAnimating();
    }

    FloatRect contentRect(FloatPoint(), layer->contentBounds());

    // fullHierarchyMatrix is the matrix that transforms objects between screen space (except projection matrix) and the most recent RenderSurfaceImpl's space.
    // nextHierarchyMatrix will only change if this layer uses a new RenderSurfaceImpl, otherwise remains the same.
    WebTransformationMatrix nextHierarchyMatrix = fullHierarchyMatrix;
    WebTransformationMatrix sublayerMatrix;

    FloatPoint renderSurfaceSublayerScale = MathUtil::computeTransform2dScaleComponents(combinedTransform);

    if (subtreeShouldRenderToSeparateSurface(layer, isScaleOrTranslation(combinedTransform))) {
        // Check back-face visibility before continuing with this surface and its subtree
        if (!layer->doubleSided() && transformToParentIsKnown(layer) && isSurfaceBackFaceVisible(layer, combinedTransform))
            return;

        if (!layer->renderSurface())
            layer->createRenderSurface();

        RenderSurfaceType* renderSurface = layer->renderSurface();
        renderSurface->clearLayerLists();

        // The owning layer's draw transform has a scale from content to layer space which we need to undo and
        // replace with a scale from the surface's subtree into layer space.
        if (!layer->contentBounds().isEmpty() && !layer->bounds().isEmpty()) {
            drawTransform.scaleNonUniform(layer->contentBounds().width() / static_cast<double>(layer->bounds().width()),
                                          layer->contentBounds().height() / static_cast<double>(layer->bounds().height()));
        }
        drawTransform.scaleNonUniform(1 / renderSurfaceSublayerScale.x(), 1 / renderSurfaceSublayerScale.y());
        renderSurface->setDrawTransform(drawTransform);

        // The origin of the new surface is the upper left corner of the layer.
        WebTransformationMatrix layerDrawTransform;
        layerDrawTransform.scaleNonUniform(renderSurfaceSublayerScale.x(), renderSurfaceSublayerScale.y());
        if (!layer->contentBounds().isEmpty() && !layer->bounds().isEmpty()) {
            layerDrawTransform.scaleNonUniform(layer->bounds().width() / static_cast<double>(layer->contentBounds().width()),
                                               layer->bounds().height() / static_cast<double>(layer->contentBounds().height()));
        }
        layer->setDrawTransform(layerDrawTransform);

        // Inside the surface's subtree, we scale everything to the owning layer's scale.
        // The sublayer matrix transforms centered layer rects into target
        // surface content space.
        sublayerMatrix.makeIdentity();
        sublayerMatrix.scaleNonUniform(renderSurfaceSublayerScale.x(), renderSurfaceSublayerScale.y());

        // The opacity value is moved from the layer to its surface, so that the entire subtree properly inherits opacity.
        renderSurface->setDrawOpacity(drawOpacity);
        renderSurface->setDrawOpacityIsAnimating(drawOpacityIsAnimating);
        layer->setDrawOpacity(1);
        layer->setDrawOpacityIsAnimating(false);

        renderSurface->setTargetSurfaceTransformsAreAnimating(animatingTransformToTarget);
        renderSurface->setScreenSpaceTransformsAreAnimating(animatingTransformToScreen);
        animatingTransformToTarget = false;
        layer->setDrawTransformIsAnimating(animatingTransformToTarget);
        layer->setScreenSpaceTransformIsAnimating(animatingTransformToScreen);

        // Update the aggregate hierarchy matrix to include the transform of the
        // newly created RenderSurfaceImpl.
        nextHierarchyMatrix.multiply(renderSurface->drawTransform());

        // The new renderSurface here will correctly clip the entire subtree. So, we do
        // not need to continue propagating the clipping state further down the tree. This
        // way, we can avoid transforming clipRects from ancestor target surface space to
        // current target surface space that could cause more w < 0 headaches.
        subtreeShouldBeClipped = false;

        if (layer->maskLayer()) {
            layer->maskLayer()->setRenderTarget(layer);
            layer->maskLayer()->setVisibleContentRect(IntRect(IntPoint(), layer->contentBounds()));
        }

        if (layer->replicaLayer() && layer->replicaLayer()->maskLayer()) {
            layer->replicaLayer()->maskLayer()->setRenderTarget(layer);
            layer->replicaLayer()->maskLayer()->setVisibleContentRect(IntRect(IntPoint(), layer->contentBounds()));
        }

        // FIXME:  make this smarter for the SkImageFilter case (check for
        //         pixel-moving filters)
        if (layer->filters().hasFilterThatMovesPixels() || layer->filter())
            nearestAncestorThatMovesPixels = renderSurface;

        // The render surface clipRect is expressed in the space where this surface draws, i.e. the same space as clipRectFromAncestor.
        if (ancestorClipsSubtree)
            renderSurface->setClipRect(clipRectFromAncestor);
        else
            renderSurface->setClipRect(IntRect());

        renderSurface->setNearestAncestorThatMovesPixels(nearestAncestorThatMovesPixels);

        renderSurfaceLayerList.push_back(layer);
    } else {
        layer->setDrawTransform(drawTransform);
        layer->setDrawTransformIsAnimating(animatingTransformToTarget);
        layer->setScreenSpaceTransformIsAnimating(animatingTransformToScreen);
        sublayerMatrix = combinedTransform;

        layer->setDrawOpacity(drawOpacity);
        layer->setDrawOpacityIsAnimating(drawOpacityIsAnimating);

        if (layer != rootLayer) {
            DCHECK(layer->parent());
            layer->clearRenderSurface();

            // Layers without renderSurfaces directly inherit the ancestor's clip status.
            subtreeShouldBeClipped = ancestorClipsSubtree;
            if (ancestorClipsSubtree)
                clipRectForSubtree = clipRectFromAncestor;

            // Layers that are not their own renderTarget will render into the target of their nearest ancestor.
            layer->setRenderTarget(layer->parent()->renderTarget());
        } else {
            // FIXME: This root layer special case code should eventually go away. https://bugs.webkit.org/show_bug.cgi?id=92290
            DCHECK(!layer->parent());
            DCHECK(layer->renderSurface());
            DCHECK(ancestorClipsSubtree);
            layer->renderSurface()->setClipRect(clipRectFromAncestor);
            subtreeShouldBeClipped = false;
        }
    }

    IntRect rectInTargetSpace = enclosingIntRect(MathUtil::mapClippedRect(layer->drawTransform(), contentRect));

    if (layerClipsSubtree(layer)) {
        subtreeShouldBeClipped = true;
        if (ancestorClipsSubtree && !layer->renderSurface()) {
            clipRectForSubtree = clipRectFromAncestor;
            clipRectForSubtree.intersect(rectInTargetSpace);
        } else
            clipRectForSubtree = rectInTargetSpace;
    }

    // Flatten to 2D if the layer doesn't preserve 3D.
    if (!layer->preserves3D())
        MathUtil::flattenTransformTo2d(sublayerMatrix);

    // Apply the sublayer transform at the center of the layer.
    sublayerMatrix.translate(0.5 * bounds.width(), 0.5 * bounds.height());
    sublayerMatrix.multiply(layer->sublayerTransform());
    sublayerMatrix.translate(-0.5 * bounds.width(), -0.5 * bounds.height());

    LayerList& descendants = (layer->renderSurface() ? layer->renderSurface()->layerList() : layerList);

    // Any layers that are appended after this point are in the layer's subtree and should be included in the sorting process.
    unsigned sortingStartIndex = descendants.size();

    if (!layerShouldBeSkipped(layer))
        descendants.push_back(layer);

    WebTransformationMatrix nextScrollCompensationMatrix = computeScrollCompensationMatrixForChildren(layer, parentMatrix, currentScrollCompensationMatrix);;

    IntRect accumulatedDrawableContentRectOfChildren;
    for (size_t i = 0; i < layer->children().size(); ++i) {
        LayerType* child = LayerTreeHostCommon::getChildAsRawPtr(layer->children(), i);
        IntRect drawableContentRectOfChildSubtree;
        calculateDrawTransformsInternal<LayerType, LayerList, RenderSurfaceType, LayerSorter>(child, rootLayer, sublayerMatrix, nextHierarchyMatrix, nextScrollCompensationMatrix,
                                                                                              clipRectForSubtree, subtreeShouldBeClipped, nearestAncestorThatMovesPixels,
                                                                                              renderSurfaceLayerList, descendants, layerSorter, maxTextureSize, deviceScaleFactor, pageScaleFactor, drawableContentRectOfChildSubtree);
        if (!drawableContentRectOfChildSubtree.isEmpty()) {
            accumulatedDrawableContentRectOfChildren.unite(drawableContentRectOfChildSubtree);
            if (child->renderSurface())
                descendants.push_back(child);
        }
    }

    // Compute the total drawableContentRect for this subtree (the rect is in targetSurface space)
    IntRect localDrawableContentRectOfSubtree = accumulatedDrawableContentRectOfChildren;
    if (layer->drawsContent())
        localDrawableContentRectOfSubtree.unite(rectInTargetSpace);
    if (subtreeShouldBeClipped)
        localDrawableContentRectOfSubtree.intersect(clipRectForSubtree);

    // Compute the layer's drawable content rect (the rect is in targetSurface space)
    IntRect drawableContentRectOfLayer = rectInTargetSpace;
    if (subtreeShouldBeClipped)
        drawableContentRectOfLayer.intersect(clipRectForSubtree);
    layer->setDrawableContentRect(drawableContentRectOfLayer);

    // Compute the layer's visible content rect (the rect is in content space)
    IntRect visibleContentRectOfLayer = calculateVisibleContentRect(layer);
    layer->setVisibleContentRect(visibleContentRectOfLayer);

    // Compute the remaining properties for the render surface, if the layer has one.
    if (layer->renderSurface() && layer != rootLayer) {
        RenderSurfaceType* renderSurface = layer->renderSurface();
        IntRect clippedContentRect = localDrawableContentRectOfSubtree;

        // Don't clip if the layer is reflected as the reflection shouldn't be
        // clipped. If the layer is animating, then the surface's transform to
        // its target is not known on the main thread, and we should not use it
        // to clip.
        if (!layer->replicaLayer() && transformToParentIsKnown(layer)) {
            // Note, it is correct to use ancestorClipsSubtree here, because we are looking at this layer's renderSurface, not the layer itself.
            if (ancestorClipsSubtree && !clippedContentRect.isEmpty()) {
                IntRect surfaceClipRect = LayerTreeHostCommon::calculateVisibleRect(renderSurface->clipRect(), clippedContentRect, renderSurface->drawTransform());
                clippedContentRect.intersect(surfaceClipRect);
            }
        }

        // The RenderSurfaceImpl backing texture cannot exceed the maximum supported
        // texture size.
        clippedContentRect.setWidth(std::min(clippedContentRect.width(), maxTextureSize));
        clippedContentRect.setHeight(std::min(clippedContentRect.height(), maxTextureSize));

        if (clippedContentRect.isEmpty())
            renderSurface->clearLayerLists();

        renderSurface->setContentRect(clippedContentRect);

        // The owning layer's screenSpaceTransform has a scale from content to layer space which we need to undo and
        // replace with a scale from the surface's subtree into layer space.
        WebTransformationMatrix screenSpaceTransform = layer->screenSpaceTransform();
        if (!layer->contentBounds().isEmpty() && !layer->bounds().isEmpty()) {
            screenSpaceTransform.scaleNonUniform(layer->contentBounds().width() / static_cast<double>(layer->bounds().width()),
                                          layer->contentBounds().height() / static_cast<double>(layer->bounds().height()));
        }
        screenSpaceTransform.scaleNonUniform(1 / renderSurfaceSublayerScale.x(), 1 / renderSurfaceSublayerScale.y());
        renderSurface->setScreenSpaceTransform(screenSpaceTransform);

        if (layer->replicaLayer()) {
            WebTransformationMatrix surfaceOriginToReplicaOriginTransform;
            surfaceOriginToReplicaOriginTransform.scaleNonUniform(renderSurfaceSublayerScale.x(), renderSurfaceSublayerScale.y());
            surfaceOriginToReplicaOriginTransform.translate(layer->replicaLayer()->position().x() + layer->replicaLayer()->anchorPoint().x() * bounds.width(),
                                                            layer->replicaLayer()->position().y() + layer->replicaLayer()->anchorPoint().y() * bounds.height());
            surfaceOriginToReplicaOriginTransform.multiply(layer->replicaLayer()->transform());
            surfaceOriginToReplicaOriginTransform.translate(-layer->replicaLayer()->anchorPoint().x() * bounds.width(), -layer->replicaLayer()->anchorPoint().y() * bounds.height());
            surfaceOriginToReplicaOriginTransform.scaleNonUniform(1 / renderSurfaceSublayerScale.x(), 1 / renderSurfaceSublayerScale.y());

            // Compute the replica's "originTransform" that maps from the replica's origin space to the target surface origin space.
            WebTransformationMatrix replicaOriginTransform = layer->renderSurface()->drawTransform() * surfaceOriginToReplicaOriginTransform;
            renderSurface->setReplicaDrawTransform(replicaOriginTransform);

            // Compute the replica's "screenSpaceTransform" that maps from the replica's origin space to the screen's origin space.
            WebTransformationMatrix replicaScreenSpaceTransform = layer->renderSurface()->screenSpaceTransform() * surfaceOriginToReplicaOriginTransform;
            renderSurface->setReplicaScreenSpaceTransform(replicaScreenSpaceTransform);
        }

        // If a render surface has no layer list, then it and none of its children needed to get drawn.
        if (!layer->renderSurface()->layerList().size()) {
            // FIXME: Originally we asserted that this layer was already at the end of the
            //        list, and only needed to remove that layer. For now, we remove the
            //        entire subtree of surfaces to fix a crash bug. The root cause is
            //        https://bugs.webkit.org/show_bug.cgi?id=74147 and we should be able
            //        to put the original assert after fixing that.
            while (renderSurfaceLayerList.back() != layer) {
                renderSurfaceLayerList.back()->clearRenderSurface();
                renderSurfaceLayerList.pop_back();
            }
            DCHECK(renderSurfaceLayerList.back() == layer);
            renderSurfaceLayerList.pop_back();
            layer->clearRenderSurface();
            return;
        }
    }

    // If neither this layer nor any of its children were added, early out.
    if (sortingStartIndex == descendants.size())
        return;

    // If preserves-3d then sort all the descendants in 3D so that they can be
    // drawn from back to front. If the preserves-3d property is also set on the parent then
    // skip the sorting as the parent will sort all the descendants anyway.
    if (descendants.size() && layer->preserves3D() && (!layer->parent() || !layer->parent()->preserves3D()))
        sortLayers(descendants.begin() + sortingStartIndex, descendants.end(), layerSorter);

    if (layer->renderSurface())
        drawableContentRectOfSubtree = enclosingIntRect(layer->renderSurface()->drawableContentRect());
    else
        drawableContentRectOfSubtree = localDrawableContentRectOfSubtree;

    if (layer->hasContributingDelegatedRenderPasses())
        layer->renderTarget()->renderSurface()->addContributingDelegatedRenderPassLayer(layer);
}

void LayerTreeHostCommon::calculateDrawTransforms(Layer* rootLayer, const IntSize& deviceViewportSize, float deviceScaleFactor, float pageScaleFactor, int maxTextureSize, std::vector<scoped_refptr<Layer> >& renderSurfaceLayerList)
{
    IntRect totalDrawableContentRect;
    WebTransformationMatrix identityMatrix;
    WebTransformationMatrix deviceScaleTransform;
    deviceScaleTransform.scale(deviceScaleFactor);

    setupRootLayerAndSurfaceForRecursion<Layer, std::vector<scoped_refptr<Layer> > >(rootLayer, renderSurfaceLayerList, deviceViewportSize);

    cc::calculateDrawTransformsInternal<Layer, std::vector<scoped_refptr<Layer> >, RenderSurface, void>(
        rootLayer, rootLayer, deviceScaleTransform, identityMatrix, identityMatrix,
        rootLayer->renderSurface()->contentRect(), true, 0, renderSurfaceLayerList,
        rootLayer->renderSurface()->layerList(), 0, maxTextureSize,
        deviceScaleFactor, pageScaleFactor, totalDrawableContentRect);
}

void LayerTreeHostCommon::calculateDrawTransforms(LayerImpl* rootLayer, const IntSize& deviceViewportSize, float deviceScaleFactor, float pageScaleFactor, LayerSorter* layerSorter, int maxTextureSize, std::vector<LayerImpl*>& renderSurfaceLayerList)
{
    IntRect totalDrawableContentRect;
    WebTransformationMatrix identityMatrix;
    WebTransformationMatrix deviceScaleTransform;
    deviceScaleTransform.scale(deviceScaleFactor);

    setupRootLayerAndSurfaceForRecursion<LayerImpl, std::vector<LayerImpl*> >(rootLayer, renderSurfaceLayerList, deviceViewportSize);

    cc::calculateDrawTransformsInternal<LayerImpl, std::vector<LayerImpl*>, RenderSurfaceImpl, LayerSorter>(
        rootLayer, rootLayer, deviceScaleTransform, identityMatrix, identityMatrix,
        rootLayer->renderSurface()->contentRect(), true, 0, renderSurfaceLayerList,
        rootLayer->renderSurface()->layerList(), layerSorter, maxTextureSize,
        deviceScaleFactor, pageScaleFactor, totalDrawableContentRect);
}

static bool pointHitsRect(const IntPoint& screenSpacePoint, const WebTransformationMatrix& localSpaceToScreenSpaceTransform, FloatRect localSpaceRect)
{
    // If the transform is not invertible, then assume that this point doesn't hit this rect.
    if (!localSpaceToScreenSpaceTransform.isInvertible())
        return false;

    // Transform the hit test point from screen space to the local space of the given rect.
    bool clipped = false;
    FloatPoint hitTestPointInLocalSpace = MathUtil::projectPoint(localSpaceToScreenSpaceTransform.inverse(), FloatPoint(screenSpacePoint), clipped);

    // If projectPoint could not project to a valid value, then we assume that this point doesn't hit this rect.
    if (clipped)
        return false;

    return localSpaceRect.contains(hitTestPointInLocalSpace);
}

static bool pointIsClippedBySurfaceOrClipRect(const IntPoint& screenSpacePoint, LayerImpl* layer)
{
    LayerImpl* currentLayer = layer;

    // Walk up the layer tree and hit-test any renderSurfaces and any layer clipRects that are active.
    while (currentLayer) {
        if (currentLayer->renderSurface() && !pointHitsRect(screenSpacePoint, currentLayer->renderSurface()->screenSpaceTransform(), currentLayer->renderSurface()->contentRect()))
            return true;

        // Note that drawableContentRects are actually in targetSurface space, so the transform we
        // have to provide is the target surface's screenSpaceTransform.
        LayerImpl* renderTarget = currentLayer->renderTarget();
        if (layerClipsSubtree(currentLayer) && !pointHitsRect(screenSpacePoint, renderTarget->renderSurface()->screenSpaceTransform(), currentLayer->drawableContentRect()))
            return true;

        currentLayer = currentLayer->parent();
    }

    // If we have finished walking all ancestors without having already exited, then the point is not clipped by any ancestors.
    return false;
}

LayerImpl* LayerTreeHostCommon::findLayerThatIsHitByPoint(const IntPoint& screenSpacePoint, std::vector<LayerImpl*>& renderSurfaceLayerList)
{
    LayerImpl* foundLayer = 0;

    typedef LayerIterator<LayerImpl, std::vector<LayerImpl*>, RenderSurfaceImpl, LayerIteratorActions::FrontToBack> LayerIteratorType;
    LayerIteratorType end = LayerIteratorType::end(&renderSurfaceLayerList);

    for (LayerIteratorType it = LayerIteratorType::begin(&renderSurfaceLayerList); it != end; ++it) {
        // We don't want to consider renderSurfaces for hit testing.
        if (!it.representsItself())
            continue;

        LayerImpl* currentLayer = (*it);

        FloatRect contentRect(FloatPoint::zero(), currentLayer->contentBounds());
        if (!pointHitsRect(screenSpacePoint, currentLayer->screenSpaceTransform(), contentRect))
            continue;

        // At this point, we think the point does hit the layer, but we need to walk up
        // the parents to ensure that the layer was not clipped in such a way that the
        // hit point actually should not hit the layer.
        if (pointIsClippedBySurfaceOrClipRect(screenSpacePoint, currentLayer))
            continue;

        foundLayer = currentLayer;
        break;
    }

    // This can potentially return 0, which means the screenSpacePoint did not successfully hit test any layers, not even the root layer.
    return foundLayer;
}

} // namespace cc
