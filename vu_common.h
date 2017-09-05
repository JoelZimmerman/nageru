#ifndef _VU_COMMON_H
#define _VU_COMMON_H 1

class QPainter;

double lufs_to_pos(float level_lu, int height, float min_level, float max_level);

void draw_vu_meter(QPainter &painter, int width, int height, int horizontal_margin, double segment_margin, bool is_on, float min_level, float max_level, bool flip, int y_offset = 0);

#endif // !defined(_VU_COMMON_H)
