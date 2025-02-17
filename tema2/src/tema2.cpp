#include <mpi.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <string.h>
#include <unordered_map>
#include <vector>
#include <fstream>
#include <sstream>
#include <climits>

#define TRACKER_RANK 0
#define MAX_FILES 10
#define MAX_FILENAME 15
#define HASH_SIZE 32
#define MAX_CHUNKS 100

using namespace std;

struct PeerData {
    int my_files_count;
    vector<string> my_files;
    int new_files_count;
    vector<string> new_files;
    unordered_map<string, unordered_map<int, string>> files; // file_names -> seg hashes
    unordered_map<string, int> seg_pos; // seg hash -> seg index
    unordered_map<int, unordered_map<string, int>> seg_map; // seg_count per file
    int trafic; // how busy a client is
};

struct TrackerData {
    unordered_map<string, unordered_map<int, string>> files; // seg of clients' files
    unordered_map<string, unordered_map<int, int>> seg; // file_name -> client and seg_count
    unordered_map<int, int> user_file_count; // file_cout -> client
    unordered_map<string, vector<int>> seeders; // file_names -> seeder clients
    int done_peer;
};

PeerData peer_data;
TrackerData tracker_data;

void receive_seeder_data(int &seeders_count, vector<int> &seeders, 
                         unordered_map<int, int> &seg_count, 
                         unordered_map<int, int> &seeders_trafic) {
    // Number of clients that own segments of the requested file                      
    MPI_Recv(&seeders_count, 1, MPI_INT, TRACKER_RANK, 200, MPI_COMM_WORLD, NULL);

    for (int i = 0; i < seeders_count; i++) {
        int seeder, seg, trafic;
        // Rank of the seeder from the tracker
        MPI_Recv(&seeder, 1, MPI_INT, TRACKER_RANK, 200, MPI_COMM_WORLD, NULL);
        seeders.push_back(seeder);

        // Number of segments that he owns
        MPI_Recv(&seg, 1, MPI_INT, TRACKER_RANK, 200, MPI_COMM_WORLD, NULL);
        seg_count[seeder] = seg;

        MPI_Recv(&trafic, 1, MPI_INT, TRACKER_RANK, 200, MPI_COMM_WORLD, NULL);
        seeders_trafic[seeder] = trafic; // how busy the peer is
    }
}

void transfer_seg(int rank, const std::string &file_name, char *send_file_name, 
                     int min_trafic_seeder, int seg_for_download) {
    for (int i = 0; i < seg_for_download; i++) {
        int seg_index = peer_data.seg_map[rank][file_name];
        // Signal the seeder to prepare for file transfer
        int ok = 1;

        // Notify the seeder about the segments transfer
        MPI_Send(&ok, 1, MPI_INT, min_trafic_seeder, 300 + min_trafic_seeder, MPI_COMM_WORLD);
        MPI_Send(send_file_name, MAX_FILENAME + 1, MPI_CHAR, min_trafic_seeder, 201 + min_trafic_seeder, MPI_COMM_WORLD);
        MPI_Send(&seg_index, 1, MPI_INT, min_trafic_seeder, 201 + min_trafic_seeder, MPI_COMM_WORLD);

        // Receive the segment hash from the seeder
        char *seg_hash = (char *)malloc(HASH_SIZE + 1);
        MPI_Recv(seg_hash, HASH_SIZE + 1, MPI_CHAR, min_trafic_seeder, 201 + min_trafic_seeder, MPI_COMM_WORLD, NULL);

        // Update the peer's data structure
        peer_data.seg_map[rank][file_name]++; // new segment added
        peer_data.files[file_name][seg_index] = seg_hash;
        peer_data.seg_pos[seg_hash] = seg_index;
    }
}

void finalize_chunk_transfer(int rank, const std::string &file_name, char *send_file_name, 
                             int min_trafic_seeder, int seg_for_download, int size, 
                             int &target_file_number) {
    // Notify the seeder to decrease its traffic load
    int decrease_trafic = -seg_for_download;
    MPI_Send(&decrease_trafic, 1, MPI_INT, min_trafic_seeder, 401, MPI_COMM_WORLD);

    // Update the segments owned and send the update to the tracker
    int seg = peer_data.seg_map[rank][file_name];
    MPI_Send(send_file_name, MAX_FILENAME + 1, MPI_CHAR, TRACKER_RANK, 500, MPI_COMM_WORLD);
    MPI_Send(&seg, 1, MPI_INT, TRACKER_RANK, 500, MPI_COMM_WORLD);

    // Check if the file download is complete
    if (seg == size) {
        // Remove the completed file from the desired list
        peer_data.new_files.erase(peer_data.new_files.begin() + target_file_number);
        peer_data.new_files_count--;

        // Save the file in the output file
        std::ostringstream filename;
        filename << "client" << rank << "_" << file_name;

        std::ofstream output_file(filename.str());
        if (!output_file) {
            std::cerr << "Eroare la deschiderea fisierului: " << filename.str() << std::endl;
            exit(-1);
        }

        // Write each segment's hash to the output file
        for (size_t i = 0; i < peer_data.files[file_name].size(); i++) {
            output_file << peer_data.files[file_name][i] << "\n";
        }

        output_file.close();
    } else {
        // Move to the next desired file
        target_file_number = (target_file_number + 1) % peer_data.new_files_count;
    }
}

void find_best_seeder(int rank, const std::vector<int> &seeders, const std::unordered_map<int, int> &seg_count,
    const std::unordered_map<int, int> &seeders_trafic, int seg, int &min_trafic, int &seg_for_download,
    int &min_trafic_seeder, int &size) {
    min_trafic = INT_MAX;
    seg_for_download = 0; // number of segments we need
    size = 0; // hhe total number of valid segments in the file

    for (const auto &seeder : seeders) {
        int trafic = seeders_trafic.at(seeder);
        int valid_seg = seg_count.at(seeder);

        if (seeder != rank && trafic < min_trafic && valid_seg > seg) {
            min_trafic = trafic;
            min_trafic_seeder = seeder;
            seg_for_download = std::min(10, valid_seg - seg);
            size = std::max(size, valid_seg);
        }
    }
}

void *download_thread_func(void *arg) {
    int rank = *(int *)arg;
    int target_file_number = 0;

    // While i still need files
    while (peer_data.new_files_count > 0) {
        // Get the desired file name
        std::string file_name = peer_data.new_files[target_file_number];
        std::vector<char> send_file_name(file_name.begin(), file_name.end());
        send_file_name.resize(MAX_FILENAME + 1, '\0'); // ensure proper null termination

        std::vector<int> seeders;
        std::unordered_map<int, int> seg_count;
        std::unordered_map<int, int> seeders_trafic;

        // Send the file name to the tracker to request seeds
        MPI_Send(send_file_name.data(), MAX_FILENAME + 1, MPI_CHAR, TRACKER_RANK, 200, MPI_COMM_WORLD);

        int seeders_count;
        // Retrieve details about the seeders from the tracker
        receive_seeder_data(seeders_count, seeders, seg_count, seeders_trafic);

        int seg = peer_data.seg_map[rank][file_name]; // how many segments of the file the peer already owns
        int min_trafic, seg_for_download, min_trafic_seeder, size;

        find_best_seeder(rank, seeders, seg_count, seeders_trafic, seg,
                         min_trafic, seg_for_download, min_trafic_seeder, size);

        // Notify the selected seeder to increase its load
        int increase_trafic = seg_for_download;
        MPI_Send(&increase_trafic, 1, MPI_INT, min_trafic_seeder, 400, MPI_COMM_WORLD);

        // Transfer the segments from the seeder
        transfer_seg(rank, file_name, send_file_name.data(), min_trafic_seeder, seg_for_download);

        // Finalize the segment transfer and update the desired file list
        finalize_chunk_transfer(rank, file_name, send_file_name.data(), min_trafic_seeder,
                                seg_for_download, size, target_file_number);
    }

    // Notify the tracker that the peer has finished downloading all files
    int ack = 1;
    MPI_Send(&ack, 1, MPI_INT, TRACKER_RANK, 501, MPI_COMM_WORLD);
    return NULL;
}

bool check_and_handle_stop_signal() {
    MPI_Status status;
    int stop_signal;

    // Check for stop signals from the tracker without blocking other operations
    int flag;
    MPI_Iprobe(TRACKER_RANK, 600, MPI_COMM_WORLD, &flag, &status);
    
    if (flag) {
        // Receive the stop signal
        MPI_Recv(&stop_signal, 1, MPI_INT, TRACKER_RANK, 600, MPI_COMM_WORLD, NULL);
        return true; // Stop signal received and processed
    }

    return false; // No stop signal found
}


void check_and_handle_load_changes() {
    MPI_Status status;
    int trafic_change;

    // Check and handle load changes
    while (true) {
        int flag = 0;
        MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &flag, &status);

        if (!flag) {
            break; // Exit loop if no messages are found
        }

        // Handle load increase
        if (status.MPI_TAG == 400) {
            MPI_Recv(&trafic_change, 1, MPI_INT, status.MPI_SOURCE, 400, MPI_COMM_WORLD, NULL);
            peer_data.trafic += trafic_change;
        }
        // Handle load decrease
        else if (status.MPI_TAG == 401) {
            MPI_Recv(&trafic_change, 1, MPI_INT, status.MPI_SOURCE, 401, MPI_COMM_WORLD, NULL);
            peer_data.trafic += trafic_change;
        } else {
            // Ignore unrelated messages
            break;
        }
    }
}


bool check_and_handle_chunk_requests(int rank) {
    MPI_Status status;
    int flag = 0;

    // Probe for incoming messages coming from different peers
    MPI_Iprobe(MPI_ANY_SOURCE, 300 + rank, MPI_COMM_WORLD, &flag, &status);
    if (!flag) {
        return false; // no messages to process
    }

    int ok; // tells whether the segment transfer can proceed
    char file_name[MAX_FILENAME + 1] = {0};
    int seg_index = 0;

    // Receive segment request data
    MPI_Recv(&ok, 1, MPI_INT, status.MPI_SOURCE, 300 + rank, MPI_COMM_WORLD, NULL);
    MPI_Recv(file_name, MAX_FILENAME + 1, MPI_CHAR, status.MPI_SOURCE, 201 + rank, MPI_COMM_WORLD, NULL);
    MPI_Recv(&seg_index, 1, MPI_INT, status.MPI_SOURCE, 201 + rank, MPI_COMM_WORLD, NULL);

    // Check if the file exists in the peer's data and if the requested segment exists within the file
    if (peer_data.files.find(file_name) == peer_data.files.end() ||
        peer_data.files[file_name].find(seg_index) == peer_data.files[file_name].end()) {
        return true; // still process the message to avoid blocking
    }

    // Extract the hash for the requested segment
    const std::string &seg_hash = peer_data.files[file_name][seg_index];

    // Send the requested segment's hash back to the requester
    MPI_Send(seg_hash.c_str(), HASH_SIZE + 1, MPI_CHAR, status.MPI_SOURCE, 201 + rank, MPI_COMM_WORLD);

    return true;
}


void *upload_thread_func(void *arg) {
    int rank = *(int*)arg;
    bool run = true;

    while (run) {
        // Check for stop signal from tracker
        if (check_and_handle_stop_signal()) {
            run = false;
            continue;
        }

        // Handle trafic management
        check_and_handle_load_changes();

        // Handle segment requests
        if (check_and_handle_chunk_requests(rank)) {
            continue;
        }
    }

    return NULL;
}

void process_file_info(int rank, int my_files_count) {
    for (int j = 0; j < my_files_count; j++) {
        // Prepare a buffer to receive the file name
        std::vector<char> file_name(MAX_FILENAME + 1, '\0');
        int file_size;

        MPI_Recv(file_name.data(), MAX_FILENAME + 1, MPI_CHAR, rank, 100, MPI_COMM_WORLD, NULL);
        MPI_Recv(&file_size, 1, MPI_INT, rank, 100, MPI_COMM_WORLD, NULL);

        std::string file_name_str(file_name.begin(), file_name.end());
        file_name_str = file_name_str.c_str(); // remove null charactrers

        // Loop through each segment of the file
        for (int k = 0; k < file_size; k++) {
            // Receive segment hash
            std::vector<char> seg_hash(HASH_SIZE + 1, '\0');
            MPI_Recv(seg_hash.data(), HASH_SIZE + 1, MPI_CHAR, rank, 100, MPI_COMM_WORLD, NULL);

            std::string seg_hash_str(seg_hash.begin(), seg_hash.end());
            seg_hash_str = seg_hash_str.c_str();

            // Update tracker data
            tracker_data.files[file_name_str][k] = seg_hash_str;
            tracker_data.seg[file_name_str][rank] = file_size;
        }

        // Add file to seeders
        tracker_data.seeders[file_name_str].push_back(rank);
    }
}


void gather_tracker_data(int numtasks) {
    // For each peer
    for (int rank = 1; rank < numtasks; rank++) {
        int my_files_count;
        MPI_Recv(&my_files_count, 1, MPI_INT, rank, 100, MPI_COMM_WORLD, NULL);

        tracker_data.user_file_count[rank] = my_files_count; // the number of files owned by the current rank 

        process_file_info(rank, my_files_count);

        // Tell the peer I received information
        int ack = 1;
        MPI_Send(&ack, 1, MPI_INT, rank, 101, MPI_COMM_WORLD);
    }
}

void handle_request_seeds(int source) {
    char file_name[MAX_FILENAME + 1];
    // Receive the name of the file for which the seeders are being requested
    MPI_Recv(file_name, MAX_FILENAME + 1, MPI_CHAR, source, 200, MPI_COMM_WORLD, NULL);

    // Retrieve the list of seeders from the tracker
    const auto &seeders = tracker_data.seeders[file_name];
    int seeders_count = seeders.size();
    
    // Send the number of seeders to the source
    MPI_Send(&seeders_count, 1, MPI_INT, source, 200, MPI_COMM_WORLD);

    // Iterate through the seeders and send their details to the source
    for (int i = 0; i < seeders_count; ++i) {
        int seeder = seeders[i];
        MPI_Send(&seeder, 1, MPI_INT, source, 200, MPI_COMM_WORLD);

        int seg = tracker_data.seg[file_name][seeder];
        MPI_Send(&seg, 1, MPI_INT, source, 200, MPI_COMM_WORLD);

        int trafic = peer_data.seg_map[seeder][file_name];
        MPI_Send(&trafic, 1, MPI_INT, source, 200, MPI_COMM_WORLD);
    }
}


void handle_update_status(int source) {
    char file_name[MAX_FILENAME + 1];

    // Receive the file name from the source
    MPI_Recv(file_name, MAX_FILENAME + 1, MPI_CHAR, source, 500, MPI_COMM_WORLD, NULL);

    // Receive the number of segments owned by the source for the file
    int seg = 0;
    MPI_Recv(&seg, 1, MPI_INT, source, 500, MPI_COMM_WORLD, NULL);

    // Update the tracker's record for the number of chunks owned by this source
    tracker_data.seg[file_name][source] = seg;
}


void handle_finished_downloading(int source) {
    // Acknowledge the source peer's downloading
    int ack = 0;
    MPI_Recv(&ack, 1, MPI_INT, source, 501, MPI_COMM_WORLD, NULL);

    // One more client is done
    ++tracker_data.done_peer;
}


void notify_all_peers_to_stop(int numtasks) {
    const int stop_signal = 1;
    // Notify all peers to stop uploading
    for (int rank = 1; rank < numtasks; ++rank) {
        MPI_Send(&stop_signal, 1, MPI_INT, rank, 600, MPI_COMM_WORLD);
    }
}


void tracker(int numtasks, int rank) {
    gather_tracker_data(numtasks);

    while (true) {
        MPI_Status status;
        int flag;

        MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &flag, &status);
        if (!flag) {
            continue;
        }

        int source = status.MPI_SOURCE;

        switch(status.MPI_TAG) {
            case 200:
                // A peer requests seeders for a specific file
                handle_request_seeds(source);
                break;
            case 500:
                // A peer reports an update on its download progress
                handle_update_status(source);
                break;
            case 501:
                // A peer notifies that it has finished downloading all desired files
                handle_finished_downloading(source);
                break;
            default:
                fprintf(stderr, "Unknown tag: %d\n", status.MPI_TAG);
                break;
        }

        // Check if all clients are finished
        if (tracker_data.done_peer == numtasks - 1) {
            notify_all_peers_to_stop(numtasks);
            break;
        }
    }
}

void read_peer_input_file(const char* filename, int rank) {
    std::ostringstream path_builder;
    path_builder << filename << rank << ".txt";
    std::string full_filename = path_builder.str();

    std::ifstream file_stream(full_filename);
    if (!file_stream) {
        std::cerr << "Eroare la deschiderea fisierului " << full_filename << std::endl;
        exit(-1);
    }

    // Number of owned files
    file_stream >> peer_data.my_files_count;

    for (int i = 0; i < peer_data.my_files_count; i++) {
        // Name of the owned files
        std::string file_name;
        int file_size;
        file_stream >> file_name >> file_size;
        peer_data.my_files.push_back(file_name);

        // Read hashes
        for (int j = 0; j < file_size; j++) {
            std::string seg_hash;
            file_stream >> seg_hash;
            peer_data.files[file_name][j] = seg_hash;
            peer_data.seg_pos[seg_hash] = j;
        }

        peer_data.seg_map[rank][file_name] = file_size;
    }

    // The number of desired files
    file_stream >> peer_data.new_files_count;
    for (int i = 0; i < peer_data.new_files_count; i++) {
        std::string file_name; // alomg with their names
        file_stream >> file_name;
        peer_data.new_files.push_back(file_name);
    }

    file_stream.close();
}

void peer(int numtasks, int rank) {
    pthread_t download_thread;
    pthread_t upload_thread;
    void *status;
    int ack;

    read_peer_input_file("in", rank);

    // Send the count of files owned by the peer to the tracker
    MPI_Send(&peer_data.my_files_count, 1, MPI_INT, TRACKER_RANK, 100, MPI_COMM_WORLD);

    // Send detailed information about each owned file to the tracker
    for (const std::string& file_name : peer_data.my_files) {
        int file_size = peer_data.files[file_name].size(); // segments number
        MPI_Send(file_name.data(), MAX_FILENAME + 1, MPI_CHAR, TRACKER_RANK, 100, MPI_COMM_WORLD);
        MPI_Send(&file_size, 1, MPI_INT, TRACKER_RANK, 100, MPI_COMM_WORLD);

        // Send the hashes for each segment of the file
        const auto& file_seg = peer_data.files[file_name];
        for (const auto& [chunk_index, seg_hash] : file_seg) {
            MPI_Send(seg_hash.data(), HASH_SIZE + 1, MPI_CHAR, TRACKER_RANK, 100, MPI_COMM_WORLD);
        }
    }

    // Wait for an ack from the tracker
    MPI_Recv(&ack, 1, MPI_INT, TRACKER_RANK, 101, MPI_COMM_WORLD, NULL);

    int r;

    r = pthread_create(&download_thread, NULL, download_thread_func, (void *) &rank);
    if (r) {
        printf("Eroare la crearea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_create(&upload_thread, NULL, upload_thread_func, (void *) &rank);
    if (r) {
        printf("Eroare la crearea thread-ului de upload\n");
        exit(-1);
    }

    r = pthread_join(download_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_join(upload_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de upload\n");
        exit(-1);
    }
}

int main (int argc, char *argv[]) {
    int numtasks, rank;

    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    if (provided < MPI_THREAD_MULTIPLE) {
        fprintf(stderr, "MPI nu are suport pentru multi-threading\n");
        exit(-1);
    }
    MPI_Comm_size(MPI_COMM_WORLD, &numtasks);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (rank == TRACKER_RANK) {
        tracker(numtasks, rank);
    } else {
        peer(numtasks, rank);
    }

    MPI_Finalize();
}