#pragma once

/** Whether the current client turn becomes durable conversation history. */
enum class CurrentTurnKind {
    Durable,
    Auxiliary,
};
