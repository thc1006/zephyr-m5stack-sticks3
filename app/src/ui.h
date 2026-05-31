/* SPDX-License-Identifier: Apache-2.0 */
#ifndef M5STICKS3_UI_H
#define M5STICKS3_UI_H

#include "pages.h"
#include "status.h"

void ui_init(void);
void ui_render(enum app_page page, const struct app_status *s);

#endif /* M5STICKS3_UI_H */
