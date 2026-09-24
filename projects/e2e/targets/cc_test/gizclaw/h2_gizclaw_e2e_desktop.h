#ifndef H2_GIZCLAW_E2E_DESKTOP_H
#define H2_GIZCLAW_E2E_DESKTOP_H

/* Initialize before desktop_main and shut down after all sessions have ended.
 * The process owner serializes these lifecycle calls. */
int h2_gizclaw_e2e_desktop_init(void);
int h2_gizclaw_e2e_desktop_shutdown(void);
int h2_gizclaw_e2e_desktop_main(int argc, char **argv);

#endif
