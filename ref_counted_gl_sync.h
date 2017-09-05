#ifndef _REF_COUNTED_GL_SYNC_H
#define _REF_COUNTED_GL_SYNC_H 1

// A wrapper around GLsync (OpenGL fences) that is automatically refcounted.
// Useful since we sometimes want to use the same fence two entirely different
// places. (We could set two fences at the same time, but they are not an
// unlimited hardware resource, so it would be a bit wasteful.)

#include <epoxy/gl.h>
#include <memory>
#include <mutex>

typedef std::shared_ptr<__GLsync> RefCountedGLsyncBase;

class RefCountedGLsync : public RefCountedGLsyncBase {
public:
	RefCountedGLsync() {}

	RefCountedGLsync(GLenum condition, GLbitfield flags) 
		: RefCountedGLsyncBase(locked_glFenceSync(condition, flags), glDeleteSync) {}

private:
	// These are to work around apitrace bug #446.
	static GLsync locked_glFenceSync(GLenum condition, GLbitfield flags)
	{
		std::lock_guard<std::mutex> lock(fence_lock);
		return glFenceSync(condition, flags);
	}

	static void locked_glDeleteSync(GLsync sync)
	{
		std::lock_guard<std::mutex> lock(fence_lock);
		glDeleteSync(sync);
	}

	static std::mutex fence_lock;
};

#endif  // !defined(_REF_COUNTED_GL_SYNC_H)
