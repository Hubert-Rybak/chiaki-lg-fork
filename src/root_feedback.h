#pragma once

/*
 * Ask Homebrew Channel's optional elevated service to activate the bundled
 * DualSense compatibility components. The request is bounded and completes
 * before SDL opens input devices, allowing the compatibility driver to claim
 * an already-connected pad without disrupting an active input session. It is
 * a no-op when the TV is not rooted, the service is unavailable, or the
 * platform is not a signature-checked match.
 */
void root_feedback_bootstrap(void);
