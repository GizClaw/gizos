#ifndef H2_LUA_TEST_POLYGON_REFERENCE_H
#define H2_LUA_TEST_POLYGON_REFERENCE_H

/* Frozen f8beec6e scan arithmetic; independent pixel sink. */
static void reference_polygon(h2_lua_job_t *job, const double *x,
                                     const double *y, size_t count,
                                     uint16_t color, double offset,
                                     int top, int bottom, int clip_left,
                                     int clip_right) {
  double intersections[128];
  float edge_x[128], edge_y[128], slope[128], error[128];
  int edge_first[128], edge_end[128];
  for (size_t i = 0; i < count; ++i) {
    size_t j = (i + 1) % count;
    edge_first[i] = (int)ceil(fmin(y[i], y[j]));
    edge_end[i] = (int)ceil(fmax(y[i], y[j]));
    edge_x[i] = (float)x[i];
    edge_y[i] = (float)y[i];
    float dx = (float)x[j] - edge_x[i], dy = (float)y[j] - edge_y[i];
    slope[i] = fabsf(dy) < 1e-5f ? 0 : dx / dy;
    error[i] = fabsf(dy) < 1e-5f ? 1 :
        32 * FLT_EPSILON * (fabsf(edge_x[i]) + fabsf(dx) *
        (1 + (fabsf(edge_y[i]) + job->display_info.height) / fabsf(dy))) + 1e-7f;
  }
  double low = y[0], high = y[0];
  for (size_t i = 1u; i < count; ++i) {
    low = fmin(low, y[i]);
    high = fmax(high, y[i]);
  }
  int first = (int)fmax(top, ceil(low));
  int last = (int)fmin(bottom - 1, floor(high));
  for (int row = first; row <= last; ++row) {
    size_t used = 0u;
    for (size_t i = 0u; i < count; ++i) {
      size_t next = (i + 1u) % count;
      if (row >= edge_first[i] && row < edge_end[i]) {
        double cross;
        if (x[i] == x[next]) cross = x[i];
        else {
          float fast = edge_x[i] + ((float)row - edge_y[i]) * slope[i];
          float fraction = fast - floorf(fast);
          if (error[i] < .25f && fraction > error[i] && fraction < 1 - error[i])
            cross = fast;
          else cross = x[i] + (row - y[i]) * (x[next] - x[i]) / (y[next] - y[i]);
        }
        size_t at = used++;
        while (at > 0u && intersections[at - 1u] > cross) {
          intersections[at] = intersections[at - 1u];
          --at;
        }
        intersections[at] = cross;
      }
    }
    /* Even-odd, half-open edges; inclusive horizontal integer spans. */
    for (size_t i = 0u; i + 1u < used; i += 2u) {
      int left = (int)floor(ceil(intersections[i]) + offset + 0.5);
      int width = (int)(floor(intersections[i + 1u]) -
                        ceil(intersections[i]) + 1);
      if (width > 0) {
        int right = left + width - 1;
        if (left < clip_left) left = clip_left;
        if (right >= clip_right) right = clip_right - 1;
        if (left <= right) {
          for (int col = left; col <= right; ++col)
            job->framebuffer[(size_t)row * job->display_info.width + col] = color;
        }
      }
    }
  }
}


#endif
