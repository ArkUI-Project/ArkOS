#ifndef ARK_CURSOR_H
#define ARK_CURSOR_H
/* Original cursor geometry shared by the guest's hardware and AA paths.
 * Coordinates are eighth-pixels in a 32 x 32 logical pixel tile. */
enum {
    ARK_CURSOR_ARROW,
    ARK_CURSOR_POINTER,
    ARK_CURSOR_TEXT,
    ARK_CURSOR_GRAB,
    ARK_CURSOR_GRABBING,
    ARK_CURSOR_RESIZE_EW,
    ARK_CURSOR_RESIZE_NS,
    ARK_CURSOR_RESIZE_NWSE,
    ARK_CURSOR_RESIZE_NESW,
    ARK_CURSOR_CROSSHAIR,
    ARK_CURSOR_COUNT
};
static inline int ark_cursor_hot_x(unsigned shape) {
    return shape == ARK_CURSOR_ARROW ? 1 : shape == ARK_CURSOR_POINTER ? 12 : 16;
}
static inline int ark_cursor_hot_y(unsigned shape) {
    return shape == ARK_CURSOR_ARROW ? 1 : shape == ARK_CURSOR_POINTER ? 2 : 16;
}
static inline int ark_cursor_polygon(int x, int y, const int p[][2], unsigned n) {
    int inside = 0;
    for (unsigned i = 0, j = n - 1; i < n; j = i++)
        if ((p[i][1] * 8 > y) != (p[j][1] * 8 > y)) {
            int at = p[i][0] * 8 + (y - p[i][1] * 8) * (p[j][0] - p[i][0]) / (p[j][1] - p[i][1]);
            if (x < at)
                inside = !inside;
        }
    return inside;
}
static inline int ark_cursor_inside(unsigned shape, int x, int y, int inner) {
    static const int arrow[2][7][2] = {
        {{1, 1}, {1, 24}, {6, 19}, {10, 28}, {14, 26}, {10, 17}, {18, 17}},
        {{2, 3}, {2, 21}, {6, 17}, {10, 25}, {12, 24}, {8, 16}, {15, 16}}};
    static const int pointer[2][16][2] = {{{10, 15},
                                           {10, 4},
                                           {12, 2},
                                           {14, 4},
                                           {14, 13},
                                           {17, 11},
                                           {20, 12},
                                           {23, 12},
                                           {25, 15},
                                           {25, 23},
                                           {22, 28},
                                           {14, 28},
                                           {7, 21},
                                           {5, 18},
                                           {6, 16},
                                           {8, 16}},
                                          {{11, 17},
                                           {11, 5},
                                           {12, 4},
                                           {13, 5},
                                           {13, 16},
                                           {17, 13},
                                           {20, 14},
                                           {23, 14},
                                           {24, 16},
                                           {24, 22},
                                           {21, 27},
                                           {15, 27},
                                           {8, 21},
                                           {7, 18},
                                           {7, 17},
                                           {9, 18}}};
    static const int grab[2][20][2] = {
        {{7, 16}, {6, 11},  {8, 9},   {10, 11}, {10, 5},  {12, 3}, {14, 5},
         {14, 3}, {16, 2},  {18, 4},  {18, 5},  {20, 4},  {22, 6}, {22, 10},
         {24, 9}, {26, 11}, {26, 21}, {22, 28}, {12, 28}, {5, 21}},
        {{9, 18},  {8, 12},  {8, 11},  {11, 14}, {11, 6},  {12, 5}, {13, 6},
         {15, 13}, {15, 4},  {16, 4},  {17, 12}, {19, 7},  {20, 6}, {21, 7},
         {21, 17}, {24, 11}, {25, 12}, {25, 20}, {21, 27}, {13, 27}}};
    static const int fist[2][12][2] = {{{7, 12},
                                        {9, 9},
                                        {13, 9},
                                        {15, 8},
                                        {20, 9},
                                        {23, 10},
                                        {25, 14},
                                        {25, 22},
                                        {22, 27},
                                        {12, 27},
                                        {6, 22},
                                        {4, 17}},
                                       {{8, 13},
                                        {10, 11},
                                        {14, 11},
                                        {16, 10},
                                        {20, 11},
                                        {22, 12},
                                        {24, 15},
                                        {24, 21},
                                        {21, 26},
                                        {13, 26},
                                        {8, 21},
                                        {6, 17}}};
    static const int ew[2][14][2] = {{{2, 16},
                                      {10, 8},
                                      {10, 13},
                                      {22, 13},
                                      {22, 8},
                                      {30, 16},
                                      {22, 24},
                                      {22, 19},
                                      {10, 19},
                                      {10, 24},
                                      {2, 16},
                                      {2, 16},
                                      {2, 16},
                                      {2, 16}},
                                     {{4, 16},
                                      {9, 11},
                                      {9, 14},
                                      {23, 14},
                                      {23, 11},
                                      {28, 16},
                                      {23, 21},
                                      {23, 18},
                                      {9, 18},
                                      {9, 21},
                                      {4, 16},
                                      {4, 16},
                                      {4, 16},
                                      {4, 16}}};
    static const int diagonal[2][10][2] = {{{5, 5},
                                            {14, 5},
                                            {11, 8},
                                            {24, 21},
                                            {27, 18},
                                            {27, 27},
                                            {18, 27},
                                            {21, 24},
                                            {8, 11},
                                            {5, 14}},
                                           {{7, 7},
                                            {11, 7},
                                            {10, 8},
                                            {24, 22},
                                            {25, 21},
                                            {25, 25},
                                            {21, 25},
                                            {22, 24},
                                            {8, 10},
                                            {7, 11}}};
    inner = inner != 0;
    switch (shape) {
    case ARK_CURSOR_ARROW:
        return ark_cursor_polygon(x, y, arrow[inner], 7);
    case ARK_CURSOR_POINTER:
        return ark_cursor_polygon(x, y, pointer[inner], 16);
    case ARK_CURSOR_GRAB:
        return ark_cursor_polygon(x, y, grab[inner], 20);
    case ARK_CURSOR_GRABBING:
        return ark_cursor_polygon(x, y, fist[inner], 12);
    case ARK_CURSOR_RESIZE_NS: {
        int t = x;
        x = y;
        y = t;
        return ark_cursor_polygon(x, y, ew[inner], 14);
    }
    case ARK_CURSOR_RESIZE_EW:
        return ark_cursor_polygon(x, y, ew[inner], 14);
    case ARK_CURSOR_RESIZE_NESW:
        x = 256 - x; /* fall through */
    case ARK_CURSOR_RESIZE_NWSE:
        return ark_cursor_polygon(x, y, diagonal[inner], 10);
    case ARK_CURSOR_TEXT: {
        int a = inner ? 1 : 0;
        return (
            (x >= (14 + a) * 8 && x < (18 - a) * 8 && y >= (5 + a) * 8 && y < (27 - a) * 8) ||
            (((y >= (5 + a) * 8 && y < (8 - a) * 8) || (y >= (24 + a) * 8 && y < (27 - a) * 8)) &&
             (x >= (10 + a) * 8 && x < (22 - a) * 8)));
    }
    case ARK_CURSOR_CROSSHAIR: {
        int a = inner ? 1 : 0;
        return (x >= (14 + a) * 8 && x < (18 - a) * 8 && y >= 4 * 8 && y < 28 * 8) ||
               (y >= (14 + a) * 8 && y < (18 - a) * 8 && x >= 4 * 8 && x < 28 * 8);
    }
    default:
        return 0;
    }
}
#endif
