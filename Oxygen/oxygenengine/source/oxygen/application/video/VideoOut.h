/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2021 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#pragma once

#include "oxygen/drawing/DrawerTexture.h"
#include "oxygen/rendering/Geometry.h"

class Renderer;
class HardwareRenderer;
class SoftwareRenderer;
class RenderParts;
class RenderResources;


class VideoOut : public SingleInstance<VideoOut>
{
public:
	VideoOut();
	~VideoOut();

	void startup();
	void shutdown();
	void reset();

	void createRenderer(bool reset);
	void destroyRenderer();
	void setActiveRenderer(bool useSoftwareRenderer, bool reset);

	inline uint32 getScreenWidth() const   { return mGameResolution.x; }
	inline uint32 getScreenHeight() const  { return mGameResolution.y; }
	inline Vec2i getScreenSize() const	   { return Vec2i(mGameResolution.x, mGameResolution.y); }
	inline Recti getScreenRect() const	   { return Recti(0, 0, mGameResolution.x, mGameResolution.y); }
	void setScreenSize(uint32 width, uint32 height);

	Vec2i getInterpolatedWorldSpaceOffset() const;

	void preFrameUpdate();
	void postFrameUpdate();
	void setInterFramePosition(float position);
	bool updateGameScreen();

	void blurGameScreen();

	void renderDebugDraw(int debugDrawMode, const Recti& rect);
	void dumpDebugDraw(int debugDrawMode);

	void toggleLayerRendering(int index);
	std::string getLayerRenderingDebugString() const;

	inline RenderParts& getRenderParts()		  { return *mRenderParts; }
	inline RenderResources& getRenderResources()  { return mRenderResources; }
	inline const std::vector<Geometry*>& getGeometries() const  { return mGeometries; }

	inline DrawerTexture& getGameScreenTexture()  { return mGameScreenTexture; }
	void getScreenshot(Bitmap& outBitmap);

private:
	void clearGeometries();
	void collectGeometries(std::vector<Geometry*>& geometries);

	void renderGameScreen();

private:
	enum class FrameState
	{
		OUTSIDE_FRAME,		// Not inside a frame simulation, and last frame was rendered (or there was no frame yet)
		INSIDE_FRAME,		// Currently inside a frame simulation
		FRAME_READY			// Last frame was completed, waiting to be rendered
	};

private:
	Renderer* mActiveRenderer = nullptr;
	HardwareRenderer* mHardwareRenderer = nullptr;
	SoftwareRenderer* mSoftwareRenderer = nullptr;

	RenderParts* mRenderParts = nullptr;
	DrawerTexture mGameScreenTexture;
	RenderResources& mRenderResources;

	Vec2i mGameResolution;
	FrameState mFrameState = FrameState::OUTSIDE_FRAME;
	uint32 mLastFrameTicks = 0;

	bool mUsingFrameInterpolation = false;
	float mInterFramePosition = 0.0f;
	Vec2i mLastWorldSpaceOffset;

	std::vector<Geometry*> mGeometries;
	GeometryFactory mGeometryFactory;
};
