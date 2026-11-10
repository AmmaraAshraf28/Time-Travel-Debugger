// ======================= TIME-TRAVEL DEBUGGER - CLIENT TEMPLATE =======================

// The client never talks to the server directly. It only talks to the load balancer.
// The load balancer wakes the server up if it is down, so the client does not care.
// The first "start" may just take a bit longer.
//
// The network is used ONLY by "start". After the .tdbg file is downloaded,
// next / prv / jmp read that file on the local disk. They never contact the load balancer.
//
// How the pieces fit together:
//   main loop -> readCommand -> runCommand
//   start -> cmdStart()  -> requestDebugFile() (Part A) -> openTdbg() (Part B) -> shows step 1
//   next, prv, jmp -> moveBy() (Part D) -> readSnapshotAt() (Part B) -> printSnapshot() (Part E)

#include <iostream>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <signal.h>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "helper.h"
using namespace std;



// ---- Constants ----
const char *PROGRAM_NAME = "ttdb";
const char *PROMPT = "ttdb> ";
const char *TDBG_FILE = "ttdb_last.tdbg"; // where the downloaded .tdbg is saved
const char *TEMP_TDBG_FILE = "ttdb_temp.tdbg";
const int MAX_JUMP = 1000000000;          // biggest n accepted by jmp
string LB_HOST = "127.0.0.0";
int LB_PORT = 5000;




// ---- Structs only the client needs ----
struct TdbgReader
{
    FILE *file_stream;
    ssize_t file_index;
    TTDBHeader header;
    vector<int64_t> index;
};



struct DebugSession
{
    int active_flag;     // false until a "start" has worked
    TdbgReader reader;   // the .tdbg file stays open for the whole session
    int64_t currentStep;
};

// =============================================================================================
// PART A: TALKING TO THE LOAD BALANCER
// =============================================================================================


int connectToHost(const char *host, int port)
{
    // Create a TCP socket and connect it to host:port.
    // Return the socket, or -1 if the connection fails.

    int client_socket = socket(AF_INET, SOCK_STREAM, 0);

    if (client_socket < 0)
    {
        return -1;
    }

    sockaddr_in server_target_addr;

    memset(&server_target_addr, 0, sizeof(server_target_addr));

    server_target_addr.sin_family = AF_INET;

    server_target_addr.sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, host, &server_target_addr.sin_addr) <= 0)
    {
        close(client_socket);

        return -1;
    }



    int connection_result = connect(client_socket, (sockaddr *)&server_target_addr, sizeof(server_target_addr));

    if (connection_result < 0)
    {
        close(client_socket);

        return -1;
    }

    return client_socket;
}




bool uploadSource(int sock, const char *sourcePath)
{
    // have to do checksum and then send
    // When the file is finished, call shutdown(sock, SHUT_WR). This is how the server knows the string is complete.
    // Return false if the file cannot be opened or any send fails.

    FILE *source_file = fopen(sourcePath, "rb");

    if (source_file == nullptr)
    {
        return false;
    }

    uint32_t upload_crc = 0;

    char upload_status_byte = 0;

    upload_crc = computeRunningCRC(upload_crc, &upload_status_byte, 1);

    if (!sendAll(sock, &upload_status_byte, 1))
    {
        fclose(source_file);

        return false;
    }

    char file_chunk_buffer[IO_BUFFER_SIZE];




    while (true)

    {
        size_t bytes_read = fread(file_chunk_buffer, 1, sizeof(file_chunk_buffer), source_file);

        if (bytes_read > 0)
        {
            upload_crc = computeRunningCRC(upload_crc, file_chunk_buffer, bytes_read);

            if (!sendAll(sock, file_chunk_buffer, bytes_read))
            {
                fclose(source_file);

                return false;
            }
        }


        if (bytes_read < sizeof(file_chunk_buffer))
        {
            break;
        }

    }

    fclose(source_file);

    if (!sendAll(sock, (const char *)&upload_crc, sizeof(uint32_t)))
    {
        return false;
    }

    if (shutdown(sock, SHUT_WR) < 0)
    {
        return false;
    }


    return true;
}





bool receiveResponse(int sock, const char *outTdbgPath, string &outError)
{
    // Reply on the wire: [status][payload][CRC-32]. The CRC covers the status byte too.
    // Feed every received byte (status included) to a TrailerChecker.
    // Status 1: collect the payload into outError. Status 0: write the payload to the file.
    // At the end call verify(). If it fails, delete the file, set outError to
    // "checksum mismatch, response corrupted" and return false.

    char status_byte = 0;

    if (!recvAll(sock, &status_byte, 1))
    {
        outError = "failed to receive response";

        return false;
    }

    TrailerChecker checker;


    checker.calculated_crc = computeRunningCRC(checker.calculated_crc, &status_byte, 1);


    FILE *output_tdbg_file = nullptr;

    if (status_byte == 0)
    {
        output_tdbg_file = fopen(outTdbgPath, "wb");

        if (output_tdbg_file == nullptr)
        {
            outError = "cannot open output file";

            return false;
        }
    }

    else
    {
        outError = "";
    }



    char stream_chunk_buffer[IO_BUFFER_SIZE];

    while (true)
    {
        ssize_t received_chunk_size = recv(sock, stream_chunk_buffer, sizeof(stream_chunk_buffer), 0);

        if (received_chunk_size < 0)
        {
            if (output_tdbg_file != nullptr)
            {
                fclose(output_tdbg_file);

                remove(outTdbgPath);
            }

            outError = "network error during receive";

            return false;
        }


        if (received_chunk_size == 0)
        {
            break;
        }


        for (ssize_t byte_pos = 0; byte_pos < received_chunk_size; byte_pos++)
        {
            bool has_payload_byte = false;

            char payload_byte = 0;

            checker.feedByte(stream_chunk_buffer[byte_pos], has_payload_byte, payload_byte);

            if (has_payload_byte)
            {
                if (status_byte == 0)
                {


                    fwrite(&payload_byte, 1, 1, output_tdbg_file);
                }
                else
                {
                    outError += payload_byte;
                }
            }
        }
    }


    if (output_tdbg_file != nullptr)
    {
        fclose(output_tdbg_file);
    }


    if (!checker.verify())
    {
        if (status_byte == 0)
        {
            remove(outTdbgPath);
        }



        outError = "checksum mismatch, response corrupted";

        return false;
    }

    if (status_byte != 0)
    {
        return false;
    }

    return true;
}






// =============================================================================================
// PART B: READING THE .tdbg FILE
// File layout (written by Phase 1):
//   header | snapshot 0 | snapshot 1 | ... | index (one 8 byte offset per snapshot)
// The index lets us jump straight to any step without reading the steps before it.
// =============================================================================================



bool openTdbg(const char *path, TdbgReader &r)
{
    r.file_stream = fopen(path, "rb");

    if (r.file_stream == nullptr)
    {
        return false;
    }

    if (fread(r.header.magic, 1, 4, r.file_stream) != 4)
    {
        fclose(r.file_stream);

        r.file_stream = nullptr;

        return false;
    }



    if (r.header.magic[0] != 'T' || r.header.magic[1] != 'T' || r.header.magic[2] != 'D' || r.header.magic[3] != 'B')
    {
        fclose(r.file_stream);

        r.file_stream = nullptr;

        return false;
    }




    if (fread(&r.header.version, sizeof(int32_t), 1, r.file_stream) != 1)
    {
        fclose(r.file_stream);

        r.file_stream = nullptr;

        return false;
    }



    if (fread(&r.header.stepCount, sizeof(int32_t), 1, r.file_stream) != 1)
    {
        fclose(r.file_stream);

        r.file_stream = nullptr;

        return false;
    }




    if (r.header.stepCount <= 0)
    {
        fclose(r.file_stream);

        r.file_stream = nullptr;

        return false;
    }



    if (fread(&r.header.indexOffset, sizeof(int64_t), 1, r.file_stream) != 1)
    {
        fclose(r.file_stream);

        r.file_stream = nullptr;

        return false;
    }



    r.index.clear();

    r.index.resize(r.header.stepCount);

    fseek(r.file_stream, r.header.indexOffset, SEEK_SET);

    size_t offsets_read_count = fread(&r.index[0], sizeof(int64_t), r.header.stepCount, r.file_stream);

    if (offsets_read_count != (size_t)r.header.stepCount)
    {
        fclose(r.file_stream);

        r.file_stream = nullptr;

        return false;
    }

    return true;
}




void closeTdbg(TdbgReader &r)
{
    // Free r.index and close the file.

    r.index.clear();

    if (r.file_stream != nullptr)
    {
        fclose(r.file_stream);

        r.file_stream = nullptr;
    }
}




Snapshot *readSnapshotAt(TdbgReader &r, int32_t step)
{
    if (step < 0 || step >= r.header.stepCount)
    {
        return nullptr;
    }

    if (r.file_stream == nullptr)
    {
        return nullptr;
    }

    int64_t snapshot_file_offset = r.index[step];

    fseek(r.file_stream, snapshot_file_offset, SEEK_SET);

    Snapshot *target_snapshot = new Snapshot();

    if (fread(&target_snapshot->stackDepth, sizeof(int32_t), 1, r.file_stream) != 1)
    {
        delete target_snapshot;

        return nullptr;
    }




    for (int32_t frame_index = 0; frame_index < target_snapshot->stackDepth; frame_index++)
    {
        Frame &current_frame = target_snapshot->callStack[frame_index];

        int32_t function_name_length = 0;

        fread(&function_name_length, sizeof(int32_t), 1, r.file_stream);

        char function_name_buffer[256];

        fread(function_name_buffer, sizeof(char), function_name_length, r.file_stream);

        function_name_buffer[function_name_length] = '\0';

        current_frame.func_name = string(function_name_buffer);

        fread(&current_frame.returnLine, sizeof(int32_t), 1, r.file_stream);

        fread(&current_frame.argc, sizeof(int32_t), 1, r.file_stream);

        for (int32_t arg_index = 0; arg_index < current_frame.argc; arg_index++)
        {
            int32_t argument_name_length = 0;

            fread(&argument_name_length, sizeof(int32_t), 1, r.file_stream);

            char argument_name_buffer[256];

            fread(argument_name_buffer, sizeof(char), argument_name_length, r.file_stream);

            argument_name_buffer[argument_name_length] = '\0';

            current_frame.argv[arg_index].name = string(argument_name_buffer);

            fread(&current_frame.argv[arg_index].value, sizeof(int32_t), 1, r.file_stream);
        }



        fread(&current_frame.localCount, sizeof(int32_t), 1, r.file_stream);

        for (int32_t local_index = 0; local_index < current_frame.localCount; local_index++)
        {
            int32_t variable_name_length = 0;

            fread(&variable_name_length, sizeof(int32_t), 1, r.file_stream);

            char variable_name_buffer[256];

            fread(variable_name_buffer, sizeof(char), variable_name_length, r.file_stream);

            variable_name_buffer[variable_name_length] = '\0';

            current_frame.locals[local_index].name = string(variable_name_buffer);

            fread(&current_frame.locals[local_index].value, sizeof(int32_t), 1, r.file_stream);
        }
    }

    return target_snapshot;
}





// =============================================================================================
// PART C: THE SESSION IN MEMORY
// There is one DebugSession variable, created in main() and passed to every command by reference.
// =============================================================================================




void initSession(DebugSession &s)
{
    s.active_flag = 0;

    s.reader.file_stream = nullptr;

    s.reader.file_index = -1;

    s.reader.index.clear();

    s.currentStep = 0;
}





void closeSession(DebugSession &s)
{
    // If active, call closeTdbg(s.reader). Then set active = false.
    // Called when the user types quit and when the input ends.

    if (s.active_flag)
    {
        closeTdbg(s.reader);

        s.active_flag = 0;
    }
}






// =============================================================================================
// PART D: THE COMMANDS
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s); // defined in Part E

bool showCurrent(DebugSession &s)
{
    // Read the snapshot at s.currentStep, call printSnapshot, delete the snapshot.
    // If readSnapshotAt returns nullptr, print "damaged .tdbg file" and return false.

    Snapshot *current_snapshot = readSnapshotAt(s.reader, (int32_t)s.currentStep);

    if (current_snapshot == nullptr)
    {
        cout << "damaged .tdbg file" << endl;

        return false;
    }

    printSnapshot((int32_t)s.currentStep, s.reader.header.stepCount, *current_snapshot);

    delete current_snapshot;

    return true;

}




bool moveBy(DebugSession &s, int32_t delta)
{
    if (!s.active_flag)
    {
        cout << "no active session" << endl;

        return false;
    }

    int64_t target_step_number = s.currentStep + delta;

    if (target_step_number < 0)
    {
        target_step_number = 0;
    }

    if (target_step_number >= s.reader.header.stepCount)
    {
        target_step_number = s.reader.header.stepCount - 1;
    }

    s.currentStep = target_step_number;

    return showCurrent(s);
}





bool cmdStart(DebugSession &s, const string &sourcePath)
{
    // 1. requestDebugFile with host and port, saving to TDBG_FILE.
    // 2. If it fails, print the error text and return false. Keep the old session as it was.
    // 3. Open the new file with openTdbg into a NEW local TdbgReader.
    //    If that fails, print "server sent a damaged .tdbg file", keep the old session, return false.
    // 4. Only now close the old reader (if s.active), then put the new reader into s,
    //    set s.currentStep = 0 and s.active = true.
    // 5. Show step 1 with showCurrent(s).

    int client_connection_sock = connectToHost(LB_HOST.c_str(), LB_PORT);

    if (client_connection_sock < 0)
    {
        cout << "failed to connect to load balancer" << endl;

        return false;
    }



    if (!uploadSource(client_connection_sock, sourcePath.c_str()))
    {
        cout << "failed to upload source file" << endl;

        close(client_connection_sock);

        return false;
    }



    string response_error_message = "";

    remove(TEMP_TDBG_FILE);

    if (!receiveResponse(client_connection_sock, TEMP_TDBG_FILE, response_error_message))
    {
        close(client_connection_sock);

        remove(TEMP_TDBG_FILE);

        if (response_error_message != "")
        {
            cout << response_error_message << endl;
        }
        else
        {
            cout << "download failed" << endl;
        }

        return false;
    }


    close(client_connection_sock);

    TdbgReader newly_opened_reader;

    newly_opened_reader.file_stream = nullptr;

    newly_opened_reader.file_index = -1;

    if (!openTdbg(TEMP_TDBG_FILE, newly_opened_reader))
    {
        cout << "server sent a damaged .tdbg file" << endl;

        remove(TEMP_TDBG_FILE);

        return false;
    }



    closeTdbg(newly_opened_reader);

    if (s.active_flag)
    {
        closeTdbg(s.reader);
    }

    remove(TDBG_FILE);

    rename(TEMP_TDBG_FILE, TDBG_FILE);


    if (!openTdbg(TDBG_FILE, s.reader))
    {
        cout << "server sent a damaged .tdbg file" << endl;

        s.active_flag = 0;

        return false;
    }


    s.currentStep = 0;

    s.active_flag = 1;

    return showCurrent(s);
}






bool cmdNext(DebugSession &s)
{
    return moveBy(s, 1);
}



bool cmdPrv(DebugSession &s)
{
    return moveBy(s, -1);
}



bool cmdJmpNext(DebugSession &s, int32_t n)
{
    return moveBy(s, n);
}



bool cmdJmpPrv(DebugSession &s, int32_t n)
{
    return moveBy(s, -n);
}

// =============================================================================================
// PART E: PRINTING
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s)
{
    cout << "Step " << (step + 1) << " / " << totalSteps << endl;

    for (int32_t frame_index = s.stackDepth - 1; frame_index >= 0; frame_index--)
    {
        const Frame &current_frame = s.callStack[frame_index];

        cout << "  [" << frame_index << "] " << current_frame.func_name << "(";

        for (int32_t arg_index = 0; arg_index < current_frame.argc; arg_index++)
        {
            cout << current_frame.argv[arg_index].name << " = " << current_frame.argv[arg_index].value;

            if (arg_index + 1 < current_frame.argc)
            {
                cout << ", ";
            }
        }

        cout << ")" << endl;

        for (int32_t local_index = 0; local_index < current_frame.localCount; local_index++)
        {
            cout << "      " << current_frame.locals[local_index].name << " = " << current_frame.locals[local_index].value << endl;
        }
    }
}




void printHelp()
{
    // Print the commands

    cout << "Available commands:" << endl;
    cout << "  start <source.bin>  : load and debug a program file" << endl;
    cout << "  next                : advance to the next step" << endl;
    cout << "  prv                 : step backward to previous step" << endl;
    cout << "  jmp next <n>        : jump forward n steps" << endl;
    cout << "  jmp prv <n>         : jump backward n steps" << endl;
    cout << "  help                : print this help text" << endl;
    cout << "  quit / exit         : quit debugger" << endl;



}







// =============================================================================================
// PART F: THE INFINITE LOOP
// =============================================================================================

bool runCommand(DebugSession &s, const string &line, bool &quitRequested)
{
    // 1. Split the line into words.
    // 2. If the first word is "ttdb", drop it.
    // 3. Match the first word:
    //      start      -> needs a path after it, then cmdStart
    //      next       -> no more words, then cmdNext
    //      prv        -> no more words, then cmdPrv
    //      jmp        -> second word "next" or "prv", third word a positive number ,
    //                    then cmdJmpNext or cmdJmpPrv
    //      help       -> printHelp
    //      quit, exit -> set quitRequested = true
    // 4. For anything else, or a wrong number of words, print "unknown command" and the help text, and return false.



    string parsed_words[16];

    int32_t word_count = 0;

    int32_t char_index = 0;

    int32_t line_length = (int32_t)line.length();

    while (char_index < line_length && word_count < 16)
    {
        while (char_index < line_length && (line[char_index] == ' ' || line[char_index] == '\t' || line[char_index] == '\r' || line[char_index] == '\n'))
        {
            char_index++;
        }

        if (char_index >= line_length)
        {
            break;
        }



        int32_t word_start_index = char_index;

        while (char_index < line_length && line[char_index] != ' ' && line[char_index] != '\t' && line[char_index] != '\r' && line[char_index] != '\n')
        {
            char_index++;
        }

        parsed_words[word_count] = line.substr(word_start_index, char_index - word_start_index);

        word_count++;
    }



    if (word_count == 0)
    {
        return true;
    }

    int32_t command_word_index = 0;

    if (parsed_words[0] == "ttdb")
    {
        command_word_index++;

        if (command_word_index >= word_count)
        {
            return true;
        }
    }
    

    string active_command_word = parsed_words[command_word_index];

    int32_t remaining_word_count = word_count - command_word_index;

    if (active_command_word == "quit" || active_command_word == "exit")
    {
        quitRequested = true;

        return true;
    }


    if (active_command_word == "help")
    {
        printHelp();

        return true;
    }

    if (active_command_word == "start")
    {
        if (remaining_word_count != 2)
        {
            cout << "unknown command" << endl;

            printHelp();

            return false;
        }

        return cmdStart(s, parsed_words[command_word_index + 1]);
    }



    if (active_command_word == "next")
    {
        if (remaining_word_count != 1)
        {
            cout << "unknown command" << endl;

            printHelp();

            return false;
        }

        return cmdNext(s);
    }



    if (active_command_word == "prv")
    {
        if (remaining_word_count != 1)
        {
            cout << "unknown command" << endl;

            printHelp();

            return false;
        }

        return cmdPrv(s);
    }



    if (active_command_word == "jmp")
    {
        if (remaining_word_count != 3)
        {
            cout << "unknown command" << endl;

            printHelp();

            return false;
        }



        string jump_direction = parsed_words[command_word_index + 1];

        int32_t jump_amount = atoi(parsed_words[command_word_index + 2].c_str());

        if (jump_amount <= 0 || jump_amount > MAX_JUMP)
        {
            cout << "unknown command" << endl;

            printHelp();

            return false;
        }


        if (jump_direction == "next")
        {
            return cmdJmpNext(s, jump_amount);
        }
        else if (jump_direction == "prv")
        {
            return cmdJmpPrv(s, jump_amount);
        }
        else
        {
            cout << "unknown command" << endl;

            printHelp();

            return false;
        }
    }

    cout << "unknown command" << endl;

    printHelp();

    return false;
}




int main(int argc, char *argv[])
{
    // you get string LB_HOST and int LB_PORT = 0 in arguments of this main function.

    if (argc >= 2)
    {
        LB_HOST = argv[1];
    }

    if (argc >= 3)
    {
        LB_PORT = atoi(argv[2]);
    }

    // DebugSession session; initSession(session);
    // Print a short welcome and the help text.
    //
    // Infinite loop:
    //   if quitRequested, break
    //
    // After the loop: closeSession(session) and return 0.
    // Never exit the loop because of an error. Only quit or the end of the input leaves it.



    DebugSession session;

    initSession(session);

    cout << "Welcome to " << PROGRAM_NAME << endl;

    printHelp();

    bool quitRequested = false;

    string input_command_line = "";

    while (!quitRequested)
    {
        cout << PROMPT;

        if (!getline(cin, input_command_line))
        {
            break;
        }

        runCommand(session, input_command_line, quitRequested);
    }



    closeSession(session);

    return 0;

    
}

