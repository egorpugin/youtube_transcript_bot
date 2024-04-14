void build(Solution &s){
    auto &t = s.addExecutable("youtube_transcript");
    t.PackageDefinitions = true;
    t += cpp23;
    t += "youtube_transcript.cpp";
    t += "org.sw.demo.nlohmann.json.natvis"_dep;
    t += "pub.egorpugin.primitives.http"_dep;
    t += "pub.egorpugin.primitives.sw.main"_dep;
    t += "org.sw.demo.tgbot"_dep;
    t += "org.sw.demo.zeux.pugixml"_dep;
}
