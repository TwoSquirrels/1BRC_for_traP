#include <bits/stdc++.h>
using namespace std;

using ll = long long;


// ========================
// Key
// ========================

struct Key {
    string channel;
    int year;
    int month;

    bool operator==(const Key& o) const {
        return year == o.year &&
               month == o.month &&
               channel == o.channel;
    }
};


struct KeyHash {
    size_t operator()(const Key& k) const {
        size_t h = hash<string>()(k.channel);
        h ^= hash<int>()(k.year) + 0x9e3779b9 + (h<<6) + (h>>2);
        h ^= hash<int>()(k.month) + 0x9e3779b9 + (h<<6) + (h>>2);
        return h;
    }
};


// ========================
// Statistics
// ========================

struct Stats {
    int mn = INT_MAX;
    int mx = INT_MIN;

    ll sum = 0;
    ll count = 0;
    ll stamps = 0;


    void add(int len, int stamp) {
        mn = min(mn, len);
        mx = max(mx, len);

        sum += len;
        count++;
        stamps += stamp;
    }


    string str() const {
        double avg = (double)sum / count;

        char buf[64];
        snprintf(
            buf,
            sizeof(buf),
            "%.2f",
            avg
        );

        return to_string(mn)
            + "/"
            + buf
            + "/"
            + to_string(mx)
            + "/"
            + to_string(count)
            + "/"
            + to_string(stamps);
    }
};


// ========================
// CSV Reader
// ========================

class CSVReader {
    ifstream fin;


public:

    CSVReader(const string& path)
        : fin(path)
    {
        string header;
        getline(fin, header);
    }


    bool next(
        long long& timestamp,
        string& channel,
        int& length,
        int& stamp
    ) {

        string line;

        if(!getline(fin,line))
            return false;


        size_t p1 = line.find(',');
        size_t p2 = line.find(',', p1+1);
        size_t p3 = line.find(',', p2+1);


        timestamp =
            stoll(line.substr(0,p1));


        channel =
            line.substr(
                p1+1,
                p2-p1-1
            );


        length =
            stoi(
                line.substr(
                    p2+1,
                    p3-p2-1
                )
            );


        stamp =
            stoi(
                line.substr(
                    p3+1
                )
            );


        return true;
    }
};


// ========================
// Aggregator
// ========================

class Aggregator {

    unordered_map<
        Key,
        Stats,
        KeyHash
    > data;


public:

    void add(
        ll timestamp,
        const string& channel,
        int length,
        int stamp
    ){

        time_t t = timestamp;
        tm *utc = gmtime(&t);


        Key key{
            channel,
            utc->tm_year + 1900,
            utc->tm_mon + 1
        };


        data[key].add(
            length,
            stamp
        );
    }


    auto& get() {
        return data;
    }
};


// ========================
// Writer
// ========================

class Writer {

public:

    static void write(
        const string& path,
        unordered_map<Key,Stats,KeyHash>& data
    ){

        ofstream fout(path);


        for(auto &[key, stat] : data){

            fout
                << key.channel
                << ","
                << formatMonth(key.year,key.month)
                << "="
                << stat.str()
                << '\n';
        }
    }


private:

    static string formatMonth(
        int y,
        int m
    ){

        char buf[32];

        snprintf(
            buf,
            sizeof(buf),
            "%04d-%02d",
            y,
            m
        );

        return buf;
    }
};


// ========================
// main
// ========================

int main(int argc,char**argv){

    if(argc != 3){
        return 1;
    }


    CSVReader reader(argv[1]);

    Aggregator agg;


    ll ts;
    string ch;
    int len,st;


    while(
        reader.next(
            ts,
            ch,
            len,
            st
        )
    ){
        agg.add(
            ts,
            ch,
            len,
            st
        );
    }


    Writer::write(
        argv[2],
        agg.get()
    );

}