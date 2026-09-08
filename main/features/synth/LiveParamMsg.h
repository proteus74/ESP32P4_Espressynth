#pragma once

// Posted by WebServer's /ws handler (WebSocket text frames from the
// browser) onto a queue drained by SynthEngine's render task, which parses
// the JSON itself and applies it to the live graph in place - no rebuild,
// no disk write. Carries the whole raw frame (not just the params sub-
// object) so the httpd/ws task stays cheap (no cJSON work there); the
// render task fully parses it (see SynthEngine::taskLoop).
struct LiveParamMsg
{
    char json[256];
};
