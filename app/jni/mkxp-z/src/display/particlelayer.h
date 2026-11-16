/*
** particlelayer.h
**
** Many small cells of one bitmap sheet drawn with a single draw call.
*/

#ifndef PARTICLELAYER_H
#define PARTICLELAYER_H

#include "disposable.h"
#include "viewport.h"

class Bitmap;
struct Color;
struct Tone;

struct ParticleLayerPrivate;

class ParticleLayer : public ViewportElement, public Disposable
{
public:
	enum { MaxParticles = 4096, StateFields = 9, MotionFields = 12 };

	ParticleLayer(Viewport *viewport = 0);
	~ParticleLayer();

	DECL_ATTR( Bitmap,    Bitmap* )
	DECL_ATTR( Opacity,   int     )
	DECL_ATTR( BlendType, int     )
	DECL_ATTR( X,         float   )
	DECL_ATTR( Y,         float   )
	DECL_ATTR( PixelSnap, bool    )
	DECL_ATTR( Smooth,    bool    )
	DECL_ATTR( Color,     Color&  )
	DECL_ATTR( Tone,      Tone&   )

	void setCell(int w, int h, int ox, int oy);
	int getCellWidth() const;
	int getCellHeight() const;
	int getCellOX() const;
	int getCellOY() const;

	/* state: n * stateStride floats, motion: n * motionStride floats or null */
	int setParticles(const float *state, int stateStride,
	                 const float *motion, int motionStride, int n);
	int addParticles(const float *state, int stateStride,
	                 const float *motion, int motionStride, int n);
	void clearParticles();
	int update(float frames);
	int getCount() const;
	int getDrawnCount() const;
	/* StateFields floats per particle */
	void exportParticles(float *out) const;

	void initDynAttribs();

	static bool available();

private:
	ParticleLayerPrivate *p;

	void draw();
	void onGeometryChange(const Scene::Geometry &);

	void releaseResources();
	const char *klassName() const { return "particle layer"; }

	ABOUT_TO_ACCESS_DISP
};

#endif // PARTICLELAYER_H
