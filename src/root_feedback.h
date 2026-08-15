#pragma once

/*
 * Ask Homebrew Channel's optional elevated service to activate the bundled
 * DualSense compatibility components. This is asynchronous and a no-op when
 * the TV is not rooted, the service is unavailable, or the platform is not a
 * signature-checked match.
 */
void root_feedback_bootstrap_async(void);
