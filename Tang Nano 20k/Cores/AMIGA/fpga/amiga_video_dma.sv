// SPDX-License-Identifier: GPL-3.0-or-later
// RGB888 transfers in aligned 16-byte SDRAM bursts. Capture and display use
// independent half-line buffers; no rounding of AGA colour components.
module amiga_video_dma(
    input wire clk,reset,
    input wire [1:0] capture_ready,
    input wire [16:0] capture_address0,capture_address1,
    input wire [6:0] capture_first0,capture_last0,capture_first1,capture_last1, // 8.11.2026: блоки окна
    output reg [1:0] capture_consumed=0,
    output wire [9:0] capture_read_address,
    input wire [31:0] capture_rgb,
    input wire request_toggle,
    input wire [22:0] request_info, // first/count 16-pixel groups, slot, row, half
    output reg request_done=0,
    output reg cache_we=0,
    // 8.11.2026: физический адрес кэша вывода (слот*276 + слово) считается здесь, по спаду, за целый
    // такт. Прежде кэш складывал cache_slot*276+cache_address сам, и сумма шла прямо в порт записи BRAM,
    // который пишет по фронту — полтакта через сумматор (как bank5 в Next до 0.37).
    output reg [10:0] cache_address=0,
    output reg [31:0] cache_data=0,
    output reg ram_cs=0,ram_we=0,
    output reg ram_double=0, // чтение двух соседних блоков по 16 байт за один слот SDRAM
    output wire [20:0] ram_address,
    output wire [127:0] ram_din,
    input wire [127:0] ram_dout,
    input wire ram_ack,ram_read_valid,
    input wire ram_single, // 8.11.2026: с ram_ack — контроллер прочитал один блок (обращение B), даже если просили пару
    input wire [1:0] ram_fetch_index,
    output wire idle,
    output reg steal=0       // видео отстаёт: придержать CPU в chip RAM
);
    reg [1:0] cap0=0,cap1=0;
    reg [2:0] req_s=0;
    reg write_busy=0,write_half=0;
    reg [16:0] write_address=0;
    reg [16:0] active_address=0;
    
    
    reg [6:0] write_burst=0,write_last=68,write_count=0;
    reg read_busy=0;
    reg [6:0] read_block=0;
    reg [6:0] read_blocks=69;
    reg [16:0] read_address=0;
    reg [10:0] unpack_address=0;
    reg ack_seen=0,owner_write=0;
    reg [2:0] turn=0,read_tail=0;
    wire [6:0] request_blocks={2'd0,request_info[17:13]}+{1'd0,request_info[17:13],1'b0};
    wire next_half=cap1[write_half]!=capture_consumed[write_half] ? write_half:!write_half;
    // 7 ноября 2026: при обоих запросах запись получает три слота видео из пяти,
    // чтение — два. Пока ждём слова последнего пакета, чтению выдавать нечего —
    // запись не ждёт.
    wire read_issue=read_busy && read_block<read_blocks;
    // Видео отстаёт: обе половины строки ждут записи (следующая готова, пока
    // пишется предыдущая), текущая половина пишется медленнее блока на 32 такта
    // clk85 (37,5 % слотов) или чтение ждёт больше 12 мкс. Тогда CPU
    // придерживается на обращениях к chip RAM (nrdy моста), как при DMA,
    // контроллер ставит видео перед портом CPU, а давно ждущее чтение идёт
    // раньше записи. Fast RAM не затронута; без отставания CPU важнее видео.
    reg [11:0] write_age=0;
    reg [10:0] read_age=0;
    wire write_backlog=&(cap1^capture_consumed);
    wire read_urgent=read_age[10];
    // 8 ноября 2026: чтение, ждущее больше 12 мкс, идёт впереди даже отстающей записи. Прежде
    // write_backlog выигрывал всегда: пока обе половины строки ждали записи, чтение не получало
    // ни одного слота, кэш вывода перескакивал строки — чёрные полосы через весь экран.
    // Опоздавшее чтение — чёрная строка, пропущенная запись — лишь прежняя половина строки.
    wire choose_write=write_busy && (!read_issue ||
        (!read_urgent && (write_backlog || turn==1 || turn==3 || turn==4)));
    always @(negedge clk) begin
        if(reset || !write_busy) write_age<=0; else if(!write_age[11]) write_age<=write_age+1'b1;
        if(reset || !read_issue) read_age<=0; else if(!read_age[10]) read_age<=read_age+1'b1;
        steal<=!reset && ((write_busy && (write_backlog || write_age[11:5]>write_count)) || read_urgent);
    end
    // Сдвоенное чтение: ещё не меньше двух блоков и пара выровнена на 32 байта
    // (8 столбцов) — тогда она внутри строки SDRAM, а второй столбец = бит 2.
    wire read_pair=read_block+7'd1<read_blocks && !read_address[0];
    assign idle=!ram_cs && !write_busy && cap1==capture_consumed;
    // The controller requests each word one clock before its GPIO output
    // register uses it. No second 128-bit payload buffer is necessary.
    assign capture_read_address={write_half,write_burst,ram_fetch_index};
    // 7 ноября 2026: кадровый буфер — блоки 0..79487, копия Kickstart — 98304..131071
    // (0x780000), у неё биты 16 и 15 оба единичные. Бит 15 гасится при бите 16: никакая
    // ошибка захвата (нет VSYNC или HSYNC, счётчики, CDC) не запишет видео в ROM —
    // лишняя строка попадёт в верх кадра и пропадёт со следующим кадром. Законных
    // адресов это не меняет: у блоков 65536..79487 бит 15 равен нулю.
    assign ram_address={2'b11,active_address[16],active_address[15]&~active_address[16],
        active_address[14:0],2'b00};
    assign ram_din={4{capture_rgb}};
    // SDRAM arbitration runs on this edge; a complete cycle remains for grants.
    always @(negedge clk) begin
        cap0<=capture_ready;cap1<=cap0;req_s<={req_s[1:0],request_toggle};
        cache_we<=0;
        if(reset) begin
            cap0<=0;cap1<=0;req_s<=0;capture_consumed<=0;request_done<=0;
            ram_cs<=0;ram_we<=0;ram_double<=0;ack_seen<=0;turn<=0;read_tail<=0;
            write_busy<=0;write_half<=0;
            read_busy<=0;cache_we<=0;
        end else begin
            if(!write_busy && cap1!=capture_consumed) begin
                // 8 ноября 2026: пишутся только блоки окна DIW (first..last), а не все 69
                write_busy<=1;write_half<=next_half;write_count<=0;
                write_burst<=next_half ? capture_first1:capture_first0;
                write_last<=next_half ? capture_last1:capture_last0;
                write_address<=(next_half ? capture_address1:capture_address0)+
                    (next_half ? capture_first1:capture_first0);

            end
            if(!read_busy && req_s[2]!=request_done) begin
                read_busy<=1;read_block<=0;
                read_address<=request_info[10:1]*17'd138+
                    (request_info[0] ? 17'd69:17'd0)+request_info[22:18]*17'd3;
                read_blocks<=request_blocks;
                unpack_address<=request_info[12:11]*11'd276+request_info[22:18]*11'd12;
            end
            if(ram_read_valid) begin
                cache_we<=1;cache_address<=unpack_address;
                cache_data<=ram_dout[31:0];unpack_address<=unpack_address+1'b1;
            end
            // Запрос завершён через 7 тактов после подтверждения последнего слота:
            // четыре слова второго пакета сдвоенного чтения приходят после него, а кэш
            // пишет каждое слово ещё через полтора такта (регистры фронта, 8.11.2026).
            if(read_tail!=0) begin
                read_tail<=read_tail-1'b1;
                if(read_tail==1)begin read_busy<=0;request_done<=req_s[2];end
            end
            if(!ram_cs) begin
                if(choose_write) begin
                    ram_cs<=1;ram_we<=1;ram_double<=0;owner_write<=1;active_address<=write_address;
                    ack_seen<=ram_ack;
                    turn<=turn==4 ? 3'd0:turn+1'b1;
                end else if(read_issue) begin
                    ram_cs<=1;ram_we<=0;ram_double<=read_pair;owner_write<=0;active_address<=read_address;
                    ack_seen<=ram_ack;turn<=turn==4 ? 3'd0:turn+1'b1;
                end
            end else if(ram_ack!=ack_seen) begin
                ram_cs<=0;
                if(owner_write) begin
                    write_address<=write_address+1'b1;
                    write_burst<=write_burst+1'b1;write_count<=write_count+1'b1;
                    if(write_burst==write_last) begin
                        capture_consumed[write_half]<=cap1[write_half];
                        write_half<=~write_half;write_busy<=0;
                    end
                end else begin
                    read_address<=read_address+(ram_double && !ram_single ? 17'd2:17'd1);
                    read_block<=read_block+(ram_double && !ram_single ? 7'd2:7'd1);
                    if(read_block+(ram_double && !ram_single ? 7'd2:7'd1)>=read_blocks) read_tail<=7;
                end
            end
        end
    end
endmodule
