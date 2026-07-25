#include "OcctMessages.h"

#include <Message.hxx>
#include <Message_Messenger.hxx>
#include <Message_PrinterOStream.hxx>

namespace viki {

void silenceOcctMessages()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    Message::DefaultMessenger()->RemovePrinters(STANDARD_TYPE(Message_PrinterOStream));
}

} // namespace viki
