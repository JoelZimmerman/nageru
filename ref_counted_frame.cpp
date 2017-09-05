#include "ref_counted_frame.h"

#include <bmusb/bmusb.h>

void release_refcounted_frame(bmusb::FrameAllocator::Frame *frame)
{
	if (frame->owner) {
		frame->owner->release_frame(*frame);
	}
	delete frame;
}
