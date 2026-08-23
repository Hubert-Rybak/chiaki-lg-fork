#pragma once

/*
 * Ask Homebrew Channel's optional elevated service to remove app-owned state
 * left by earlier experimental compatibility builds. The bounded request runs
 * before SDL opens input devices. It is a no-op on unrooted or clean TVs.
 */
void root_feedback_bootstrap(void);
