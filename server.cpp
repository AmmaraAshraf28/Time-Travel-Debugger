    // ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

    // Pipeline this file implements, top to bottom:
    //   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
    //   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
    //   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
    //   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
    //   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)


    #include <iostream>
    #include <string>
    #include <cstdint>
    #include <fstream>
    #ifndef _WIN32
    #include <unistd.h>
    #include <sys/socket.h>
    #endif
    #include <cstdint>
    #include <cstdio>
    using namespace std;

    // ---- Constants ----
    const int32_t MAX_VARS_PER_FRAME = 16;
    const int32_t MAX_STACK_DEPTH = 64;
    const int32_t MAX_FUNCS = 128;
    const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
    const int32_t MAX_PATCHES = MAX_FUNCS * 4;
    const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
    const int32_t IO_BUFFER_SIZE = 64 * 1024;                  // fixed buffer for streaming to/from disk
    const int32_t SOCKET_TIMEOUT_SEC = 5;                      // TODO: apply as SO_RCVTIMEO so a deadclient can't hang the server forever

    // ---- Custom data structures

    // Stack: back the live Call Stack during execution
    template <typename T>
    class Stack
    {
        struct Node
        {
            T data;
            Node *next;
        };
        Node *top;
        int32_t count;

    public:
        // Implement these functions:
        Stack()
        { 

            top = nullptr;
            count = 0;

        }


        void push(const T &val)
        {
            // pushes the value on the stack if max limit is not reached yet.
            if (count < MAX_STACK_DEPTH)
            {

                Node* newNode = new Node();

                newNode->data = val;
                newNode->next = top;

                top = newNode;
                count++;

            }
        }


        T pop()
        {
            // pop the top value on the stack

            if (isEmpty()) 
            {
                throw runtime_error("Stack is empty");
            }
            
            Node* temp = top;

            T popped_Val = temp->data;
            top = top->next;

            delete temp;

            count--;

            return popped_Val;
        }


        T &peek()
        {
            // returns the top value on the stack

            if (isEmpty()) 
            {

                throw runtime_error("Stack is  empty");
            }

            return top->data;
        }


        bool isEmpty()
        {

            return count == 0;
        }


        int32_t depth()
        {

            return count;
        }


        int32_t snapshot_into(T out[], int32_t maxLen)
        {
            // copies every frame, top to bottom in the array given as a parameter
            // this is what buildSnapshot() call, returns count written
            
            int32_t written = 0;

            Node* current = top;

            while (current != nullptr && written < maxLen)
            {

                out[written] = current->data;

                current = current->next;

                written++;

            }
            
            return written;
        }

    };


    // Timeline : doubly linked list of Snapshots
    struct Snapshot; // fwd declaration;

    struct TimelineNode
    {
        Snapshot *data;

        TimelineNode *next;
        TimelineNode *prev;
    };

    class Timeline
    {
        TimelineNode *head, *tail;
        
        int32_t stepCount;

    public:
        // Implement these functions
        Timeline()
        {

            head = nullptr;
            tail = nullptr;
            stepCount = 0;

        }



        void record(Snapshot *s)
        {
            // add record in the timeline
            TimelineNode* newNode = new TimelineNode();

            newNode->data = s;

            newNode->next = nullptr;
            newNode->prev = tail;


            if (tail != nullptr)
            {
                tail->next = newNode;
            } 
            else
            {
                head = newNode; // If list is empty , new node is also head
            }
            

            tail = newNode;
            stepCount++;
        }



        TimelineNode *begin()
        {
            return head;
        }



        int32_t getStepCount()
        {

            return stepCount;
        }
    };


    // Core structs
    struct Variable
    {
        string name;
        int32_t value;
    };


    struct Frame
    {
        string func_name;
        int32_t argc;
        Variable argv[MAX_VARS_PER_FRAME];
        int32_t returnLine;
        Variable locals[MAX_VARS_PER_FRAME];
        int32_t localCount;
    };


    struct Snapshot
    {
        Frame callStack[MAX_STACK_DEPTH];
        int32_t stackDepth;
    };


    struct TTDBHeader
    {
        char magic[4]; // "TTDB"
        int32_t version;
        int32_t stepCount;
        int64_t indexOffset;
    };


    void writeHeader(FILE *f, const TTDBHeader &h)
    {

        fwrite(h.magic, 1, 4, f);

        fwrite(&h.version, sizeof(int32_t), 1, f);

        fwrite(&h.stepCount, sizeof(int32_t), 1, f);
        

        fwrite(&h.indexOffset, sizeof(int64_t), 1, f);

    }


    // resolve.bin - bookkeeping
    struct FuncEntry
    {
        string funcName;
        int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
    };


    struct PendingPatch
    {
        int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
        string targetFuncName;
    };


    // PASS 0x0: READING source.bin + VALIDITY CHECK
    bool readSourceLine(ifstream &in, string &out)
    {

        while (getline(in, out))
        {
            bool isBlank = true;
            int32_t line_len = (int32_t)out.length();

            for (int i = 0; i < line_len; i++)
            {

                if (out[i] != ' ' && out[i] != '\t' && out[i] != '\r') 
                {

                    isBlank = false;
                    break; 
                }
            }


            
            if (isBlank == false)
            {
                return true; 
            }
        }

        return false; 
    }


    string firstWord(const string &line)
    {

        string word = "";

        int32_t line_len = (int32_t)line.length();
        int i = 0;
        
        while (i < line_len && line[i] == ' ')
        {
            i++;
        }

        
        while (i < line_len && line[i] != ' ')
        {

            word = word + line[i];
            i++;
        }
        
        return word;
    }


    string secondWord(const string &line)
    {

        string word = "";

        int32_t line_len = (int32_t)line.length();

        int i = 0;
        
        while (i < line_len && line[i] == ' ')
        {
            i++;
        }


        while (i < line_len && line[i] != ' ')
        {
            i++;
        }


        while (i < line_len && line[i] == ' ') 
        {
            i++;
        }
        

        while (i < line_len && line[i] != ' ')
        {
            word = word + line[i];
            i++;
        }
        
        return word;


    }


    bool validateProgram(const char *sourcePath)
    {
        ifstream file(sourcePath);

        if (!file.is_open())
        {
            
            return false; 
        }
        

        string line;
        
        bool insideFunction = false; 
        
        while (readSourceLine(file, line))
        {
            
            string keyword = firstWord(line);
            
            if (keyword == "FUNC")
            {
                if (insideFunction == true) 
                {

                    file.close();
                    return false; 
                }

                insideFunction = true; 

            } 
            else
            {
                if (keyword == "FUNC_END")
                {

                    if (insideFunction == false) 
                    {
                        file.close();
                        return false; 
                    }

                    insideFunction = false; 

                }
            }
        }
        

        file.close();
        
        if (insideFunction == true)
        {

            return false;
        }
        

        return true; 
        
    }


    int64_t writeResolveRecord(FILE *f, int64_t offsetField, const string &text)
    {

        int64_t recordStart = ftell(f);
        

        fwrite(&offsetField, sizeof(int64_t), 1, f);
    

        int32_t size = text.length();

        fwrite(&size, sizeof(int32_t), 1, f);
        

        fwrite(text.c_str(), 1, size, f);
        
        return recordStart;

    }


    int64_t readResolveRecord(FILE *f, string &outText)
    {

        int64_t offsetField = 0;
        int32_t size = 0;
        

        if (fread(&offsetField, sizeof(int64_t), 1, f) == 0)
        {

            return -1; 
        }
        

        fread(&size, sizeof(int32_t), 1, f);


        outText = "";

        for (int i = 0; i < size; i++)
        {

            char c;

            fread(&c, 1, 1, f);
            outText = outText + c;

        }
        
        return offsetField;
    }


    int64_t resolveProgram(const char *sourcePath, const char *resolveBinPath)
    {
        FuncEntry funcArray[MAX_FUNCS];

        int32_t funcCount = 0;

        PendingPatch patches[MAX_PATCHES];
        int32_t patchCount = 0;


        ifstream in(sourcePath);

        FILE *out = fopen(resolveBinPath, "wb+"); 
        
        string line;
        
    
        while (readSourceLine(in, line))
        {
            
            string keyword = firstWord(line);
            
            if (keyword == "FUNC")
            {
                string funcName = secondWord(line);
                
        
                int64_t position = ftell(out);
                
            
                funcArray[funcCount].funcName = funcName;
                
                funcArray[funcCount].byteOffsetInResolveBin = position;
                funcCount++;
                
            
                writeResolveRecord(out, 0, line);
            }
            else
            {
                if (keyword == "CALL")
                {
                    string targetFunc = secondWord(line);
                    
            
                    int64_t position = writeResolveRecord(out, 0, line);
                    
            
                    patches[patchCount].byteOffsetOfOffsetField = position;
                    
                    patches[patchCount].targetFuncName = targetFunc;

                    patchCount++;

                }
                else 
                {
                
                    writeResolveRecord(out, 0, line);
                }
            }
        }
        

    
        for (int i = 0; i < patchCount; i++)
        {
            
            int64_t targetOffset = -1;
            
        
            for (int j = 0; j < funcCount; j++)
            {

                if (funcArray[j].funcName == patches[i].targetFuncName) 
                {

                    targetOffset = funcArray[j].byteOffsetInResolveBin;

                    break;
                }
            }
            
            
            fseek(out, patches[i].byteOffsetOfOffsetField, SEEK_SET);
            
            
            fwrite(&targetOffset, sizeof(int64_t), 1, out);
        }
        
        
        int64_t mainOffset = -1;
        
        for (int i = 0; i < funcCount; i++) 
        {

            if (funcArray[i].funcName == "main")
            {
                mainOffset = funcArray[i].byteOffsetInResolveBin;
                break;
            }

        }
        
    
        fclose(out);

        in.close();
        
        return mainOffset;

    }



    // PASS 0x2: EXECUTION (tokenization happens here)
    enum TokenType
    {
        KEYWORD,
        IDENTIFIER,
        PARAM
    };



    struct Token
    {
        TokenType type;
        string text;
    };



    int32_t tokenizeLine(const string &line, Token tokens[], int32_t maxTokens)
    {
        
        int32_t token_count = 0;
        int32_t line_len = line.length();
        int32_t char_idx = 0;

        while (char_idx < line_len && token_count < maxTokens)
        {
            
            while (char_idx < line_len && (line[char_idx] == ' ' || line[char_idx] == '\t' || line[char_idx] == '\r'))
            {

                char_idx++;
            }

            
            if (char_idx >= line_len)
            {
                break;
            }

            
            string current_word = "";

            while (char_idx < line_len && line[char_idx] != ' ' && line[char_idx] != '\t' && line[char_idx] != '\r')
            {

                current_word += line[char_idx];
                char_idx++;
            }

            
            if (current_word.length() > 0)
            {
            
                tokens[token_count].text = current_word;

                if (token_count == 0)
                {

                    tokens[token_count].type = KEYWORD;
                }
                else
                {
                    if (token_count == 1)
                    {

                        tokens[token_count].type = IDENTIFIER;
                    }
                    else
                    {
                    
                        tokens[token_count].type = PARAM;
                    }
                }

                token_count++;
            }
        }

        return token_count;


    }


    Snapshot *buildSnapshot(Stack<Frame> &callStack)
    {

        Snapshot *new_snapshot_ptr = new Snapshot();

        new_snapshot_ptr->stackDepth = callStack.depth();

    
        callStack.snapshot_into(new_snapshot_ptr->callStack, MAX_STACK_DEPTH);

        return new_snapshot_ptr;

    }


    int32_t parseInteger(const string &str_val)
    {


        int32_t result_num = 0;
        int32_t sign_mult = 1;
        int32_t start_pos = 0;

        if (str_val.length() > 0 && str_val[0] == '-')
        {

            sign_mult = -1;
            start_pos = 1;
        }


        int32_t length = (int32_t)str_val.length();

        for (int32_t i = start_pos; i < length; i++)
        {

            if (str_val[i] >= '0' && str_val[i] <= '9')
            {

                result_num = (result_num * 10) + (str_val[i] - '0');
            }
        }

        return result_num * sign_mult;
    }


    bool isNumberLiteral(const string &str_val)
    {
        if (str_val.empty())
        {

            return false;
        }
        
        int32_t length = (int32_t)str_val.length();

        int32_t start_pos = 0;

        if (str_val[0] == '-')
        {
            start_pos = 1;

            if (length == 1)
            {

                return false;
            }
        }


        for (int32_t i = start_pos; i < length; i++)
        {

            if (str_val[i] < '0' || str_val[i] > '9')
            {

                return false;
            }
        }

        return true;

    }



    int32_t getOperandValue(Frame &active_frame, const string &operand_str)
    {

        if (isNumberLiteral(operand_str))
        {
            
            return parseInteger(operand_str);
        }


        for (int32_t i = 0; i < active_frame.localCount; i++)
        {

            if (active_frame.locals[i].name == operand_str)
            {

                return active_frame.locals[i].value;
            }

        }


        for (int32_t i = 0; i < active_frame.argc; i++)
        {

            if ( active_frame.argv[i].name == operand_str)
            {

                return  active_frame.argv[i].value;
            }
        }

        return 0;

    }



    void setLocalVariable(Frame &active_frame, const string &var_name, int32_t new_val)
    {

        for (int32_t i = 0; i < active_frame.localCount; i++)
        {

            if (active_frame.locals[i].name == var_name)
            {

                active_frame.locals[i].value = new_val;
                return;
            }
        }

        if (active_frame.localCount < MAX_VARS_PER_FRAME)
        {

            active_frame.locals[active_frame.localCount].name = var_name;
            
            active_frame.locals[active_frame.localCount].value = new_val;
            
            active_frame.localCount++;
        }
    }


    void executeProgram(const char *resolveBinPath, int64_t mainOffset, Timeline &timeline)
    {

        if (mainOffset < 0)
        {

            return;
        }


        FILE *bin_file = fopen(resolveBinPath, "rb");

        if (bin_file == nullptr)
        {

            return;
        }

        Stack<Frame> call_stack;

        fseek(bin_file, mainOffset, SEEK_SET);

        string initial_line = "";

        readResolveRecord(bin_file, initial_line);

        Frame main_frame;

        main_frame.func_name = "main";
        main_frame.argc = 0;
        main_frame.localCount = 0;
        main_frame.returnLine = -1;



        call_stack.push(main_frame);

        Snapshot *first_snapshot = buildSnapshot(call_stack);

        timeline.record(first_snapshot);

        Token token_buffer[MAX_TOKENS];

        while (!call_stack.isEmpty())
        {
            string current_code_line = "";

            int64_t next_target_offset = readResolveRecord(bin_file, current_code_line);

            if (current_code_line == "")
            {
                break;
            }


            int32_t total_tokens = tokenizeLine(current_code_line, token_buffer, MAX_TOKENS);

            if (total_tokens == 0)
            {
                continue;
            }


            string cmd_keyword = token_buffer[0].text;

            if (cmd_keyword == "SET")
            {

                if (total_tokens >= 3)
                {

                    string target_var = token_buffer[1].text;

                    int32_t assigned_val = getOperandValue(call_stack.peek(), token_buffer[2].text);
                    
                    setLocalVariable(call_stack.peek(), target_var, assigned_val);
                }
            }
            else
            {
                if (cmd_keyword == "ADD")
                {
                    if (total_tokens >= 3)
                    {
                        string target_var = token_buffer[1].text;
                        
                        int32_t current_val = getOperandValue(call_stack.peek(), target_var);
                        
                        int32_t add_val = getOperandValue(call_stack.peek(), token_buffer[2].text);
                    
                        setLocalVariable(call_stack.peek(), target_var, current_val + add_val);
                    
                    }
                }
                else
                {
                    if (cmd_keyword == "SUB")
                    {
                        if (total_tokens >= 3)
                        {
                            string target_var = token_buffer[1].text;
                        
                            int32_t current_val = getOperandValue(call_stack.peek(), target_var);
                        
                            int32_t sub_val = getOperandValue(call_stack.peek(), token_buffer[2].text);
                            
                            setLocalVariable(call_stack.peek(), target_var, current_val - sub_val);
                    
                        }
                    }
                    else
                    {
                        if (cmd_keyword == "MUL")
                        {
                            if (total_tokens >= 3)
                            {
                                
                                string target_var = token_buffer[1].text;
                                
                                int32_t current_val = getOperandValue(call_stack.peek(), target_var);
                                
                                int32_t mul_val = getOperandValue(call_stack.peek(), token_buffer[2].text);
                            
                                setLocalVariable(call_stack.peek(), target_var, current_val * mul_val);
                            
                            }
                        }
                        else
                        {
                            if (cmd_keyword == "DIV")
                            {
                                if (total_tokens >= 3)
                                {
                                
                                    string target_var = token_buffer[1].text;
                                    
                                    int32_t current_val = getOperandValue(call_stack.peek(), target_var);
                                
                                    int32_t div_val = getOperandValue(call_stack.peek(), token_buffer[2].text);

                                    if (div_val != 0)
                                    {
                                        
                                        setLocalVariable(call_stack.peek(), target_var, current_val / div_val);
                                    }
                                }
                            }
                            else
                            {
                                if (cmd_keyword == "CALL")
                                {
                                    int64_t return_offset = ftell(bin_file);

                                    Frame child_frame;
                                    child_frame.func_name = token_buffer[1].text;
                                    child_frame.argc = 0;
                                    child_frame.localCount = 0;
                                    child_frame.returnLine = return_offset;

                                    for (int32_t param_idx = 2; param_idx < total_tokens && child_frame.argc < MAX_VARS_PER_FRAME; param_idx++)
                                    {

                                        child_frame.argv[child_frame.argc].name = "arg_" + to_string(child_frame.argc);
                                    
                                        child_frame.argv[child_frame.argc].value = getOperandValue(call_stack.peek(), token_buffer[param_idx].text);
                                        
                                        child_frame.argc++;

                                    }

                                    call_stack.push(child_frame);

                                    if (next_target_offset > 0)
                                    {
                                        fseek(bin_file, next_target_offset, SEEK_SET);
                                    
                                        string func_header_line = "";
                                        
                                        readResolveRecord(bin_file, func_header_line);
                                    }
                                }
                                else
                                {
                                    if (cmd_keyword == "FUNC_END")
                                    {
                                        Frame completed_frame = call_stack.pop();

                                        if (completed_frame.returnLine >= 0)
                                        {
                                            fseek(bin_file, completed_frame.returnLine, SEEK_SET);
                                        }
                                        else
                                        {
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            Snapshot *step_snapshot = buildSnapshot( call_stack);

            timeline.record(step_snapshot);
        }

        fclose(bin_file);
    }


    void writeTdbg(Timeline &timeline, const char *tdbgPath)
    {
        FILE *session_file = fopen(tdbgPath, "wb");

        if (session_file == nullptr)
        {
            return;
        }

        TTDBHeader file_header;

        file_header.magic[0] = 'T';
        file_header.magic[1] = 'T';
        file_header.magic[2] = 'D';
        file_header.magic[3] = 'B';

        file_header.version = 1;

        file_header.stepCount = timeline.getStepCount();

        file_header.indexOffset = 0;

        writeHeader(session_file, file_header);

        int32_t total_steps = timeline.getStepCount();

        int64_t *step_file_offsets = new int64_t[total_steps > 0 ? total_steps : 1];
        
        int32_t current_step_number = 0;

        TimelineNode *current_timeline_node = timeline.begin();

        while (current_timeline_node != nullptr && current_step_number < total_steps)
        {
            int64_t current_snapshot_file_position = ftell(session_file);


            step_file_offsets[current_step_number] = current_snapshot_file_position;

            Snapshot *snapshot_to_write = current_timeline_node->data;

            int32_t current_stack_depth = snapshot_to_write->stackDepth;


            
            fwrite(&current_stack_depth, sizeof(int32_t), 1, session_file);

            for (int32_t frame_index = 0; frame_index < current_stack_depth; frame_index++)
            {

                Frame &current_frame = snapshot_to_write->callStack[frame_index];

                int32_t function_name_length = (int32_t)current_frame.func_name.length();
                    const char *function_name_characters = current_frame.func_name.c_str();
                
                fwrite(&function_name_length, sizeof(int32_t), 1, session_file);
                
                fwrite(function_name_characters, sizeof(char), function_name_length, session_file);

                int32_t return_line_offset = current_frame.returnLine;
                
                fwrite(&return_line_offset, sizeof(int32_t), 1, session_file);

                int32_t argument_count = current_frame.argc;
                
                fwrite(&argument_count, sizeof(int32_t), 1, session_file);

                for (int32_t arg_index = 0; arg_index < argument_count; arg_index++)
                {

                    int32_t argument_name_length = (int32_t)current_frame.argv[arg_index].name.length();
                    const char *argument_name_characters = current_frame.argv[arg_index].name.c_str();
                    
                    int32_t argument_value = current_frame.argv[arg_index].value;

                    fwrite(&argument_name_length, sizeof(int32_t), 1, session_file);
                    fwrite(argument_name_characters, sizeof(char), argument_name_length, session_file);
                    
                    fwrite(&argument_value, sizeof(int32_t), 1, session_file);
                }


                int32_t local_variable_count = current_frame.localCount;
                
                fwrite(&local_variable_count, sizeof(int32_t), 1, session_file);

                for (int32_t local_index = 0; local_index < local_variable_count; local_index++)
                {

                    int32_t variable_name_length = (int32_t)current_frame.locals[local_index].name.length();
                    const char *variable_name_characters = current_frame.locals[local_index].name.c_str();
                        int32_t variable_value = current_frame.locals[local_index].value;

                    fwrite(&variable_name_length, sizeof(int32_t), 1, session_file);
                    fwrite(variable_name_characters, sizeof(char), variable_name_length, session_file);
                    fwrite(&variable_value, sizeof(int32_t), 1, session_file);
                
                
                }
            }

            current_step_number++;
            current_timeline_node = current_timeline_node->next;
        }


        int64_t index_table_file_position = ftell(session_file);

        for (int32_t step_index = 0; step_index < total_steps; step_index++)
        {

            int64_t offset_value_to_write = step_file_offsets[step_index];
            fwrite(&offset_value_to_write, sizeof(int64_t), 1, session_file);
        }

        fseek(session_file, 0, SEEK_SET);

        file_header.indexOffset = index_table_file_position;

        writeHeader(session_file, file_header);

        delete[] step_file_offsets;
        fclose(session_file);
        
    }


    // main section
    int32_t main()
    {

        if (!validateProgram("source.bin"))
        {
            // send an error response instead of a .tdbg file
            return 1;
        }

        int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");

        Timeline timeline;
        executeProgram("resolve.bin", mainOffset, timeline);

        writeTdbg(timeline, "session.tdbg");
        cout << "Program checking" << endl;

        return 0;
    }