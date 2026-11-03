#pragma once

// Static help page: fetches the embedded README (see /help.md, served from
// README_MD in generated readme_md.h) and renders it client-side with a
// small dependency-free Markdown renderer. No CDN/network dependency, so it
// works fully offline (e.g. while connected to the WiFiManager AP portal).
//
// Scoped to exactly the Markdown subset actually used in this project's
// README.md: headings, bold/italic, inline code, code fences, tables,
// blockquotes, hr, ordered/unordered lists, links, paragraphs. No nested
// lists, no raw HTML passthrough (raw-HTML blocks in the source — picture
// galleries, badges — sit between <!-- help:skip --> markers and are
// stripped at build time, see tools/version.py).
static const char HELP_PAGE_HTML[] PROGMEM = R"HELPPAGE(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>WeatherNode Help</title>
<style>
  body{font-family:sans-serif;max-width:800px;margin:1rem auto;padding:0 1rem;line-height:1.5;}
  table{border-collapse:collapse;margin:1rem 0;width:100%;}
  th,td{border:1px solid #ccc;padding:4px 8px;text-align:left;}
  pre{background:#f4f4f4;padding:0.5rem;overflow-x:auto;}
  code{background:#f4f4f4;padding:0 3px;}
  pre code{background:none;padding:0;}
  blockquote{border-left:3px solid #ccc;margin:0.5rem 0;padding:0.2rem 1rem;color:#555;}
  h1,h2,h3{border-bottom:1px solid #eee;padding-bottom:0.2rem;}
</style>
</head>
<body>
<p><a href="/">&larr; Back</a></p>
<div id="content">Loading...</div>
<script>
function escapeHtml(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');}
// Matches GitHub's own heading-slug algorithm: each space becomes its own
// hyphen (runs of spaces are NOT collapsed), so ids agree with the README's
// own markdown links whether viewed on GitHub/git server or on this page —
// e.g. "... + ..." leaves a double space after removing "+", which must
// become "--", not "-".
function slugify(t){return t.toLowerCase().replace(/[^a-z0-9 -]/g,'').trim().replace(/ /g,'-');}
function renderInline(t){
  t=escapeHtml(t);
  t=t.replace(/`([^`]+)`/g,function(_,c){return '<code>'+c+'</code>';});
  t=t.replace(/\[([^\]]+)\]\(([^)]+)\)/g,'<a href="$2">$1</a>');
  t=t.replace(/\*\*([^*]+)\*\*/g,'<strong>$1</strong>');
  t=t.replace(/\*([^*]+)\*/g,'<em>$1</em>');
  return t;
}
function splitRow(l){var s=l.trim().replace(/^\|/,'').replace(/\|$/,'');return s.split('|').map(function(c){return c.trim();});}
function renderTable(rows){
  var o='<table><thead><tr>';
  o+=rows[0].map(function(c){return '<th>'+renderInline(c)+'</th>';}).join('');
  o+='</tr></thead><tbody>';
  for(var r=2;r<rows.length;r++){o+='<tr>'+rows[r].map(function(c){return '<td>'+renderInline(c)+'</td>';}).join('')+'</tr>';}
  o+='</tbody></table>';
  return o;
}
function renderMarkdown(md){
  var lines=md.replace(/\r\n/g,'\n').split('\n');
  var html=[],i=0,inCode=false,codeBuf=[],listType=null,tableBuf=[];
  function closeList(){if(listType){html.push('</'+listType+'>');listType=null;}}
  function closeTable(){if(tableBuf.length){html.push(renderTable(tableBuf));tableBuf=[];}}
  while(i<lines.length){
    var line=lines[i];
    if(/^```/.test(line)){
      closeList();closeTable();
      if(!inCode){inCode=true;codeBuf=[];i++;continue;}
      inCode=false;
      html.push('<pre><code>'+escapeHtml(codeBuf.join('\n'))+'</code></pre>');
      i++;continue;
    }
    if(inCode){codeBuf.push(line);i++;continue;}
    if(/^\s*\|.*\|\s*$/.test(line)){tableBuf.push(splitRow(line));i++;continue;}
    if(tableBuf.length)closeTable();
    if(line.trim()===''){closeList();i++;continue;}
    if(/^-{3,}$/.test(line.trim())){closeList();html.push('<hr>');i++;continue;}
    var h=line.match(/^(#{1,6})\s+(.*)$/);
    if(h){
      closeList();
      var level=h[1].length,text=h[2];
      html.push('<h'+level+' id="'+slugify(text)+'">'+renderInline(text)+'</h'+level+'>');
      i++;continue;
    }
    var bq=line.match(/^>\s?(.*)$/);
    if(bq){closeList();html.push('<blockquote>'+renderInline(bq[1])+'</blockquote>');i++;continue;}
    var ul=line.match(/^[-*]\s+(.*)$/);
    if(ul){
      if(listType!=='ul'){closeList();html.push('<ul>');listType='ul';}
      html.push('<li>'+renderInline(ul[1])+'</li>');
      i++;continue;
    }
    var ol=line.match(/^\d+\.\s+(.*)$/);
    if(ol){
      if(listType!=='ol'){closeList();html.push('<ol>');listType='ol';}
      html.push('<li>'+renderInline(ol[1])+'</li>');
      i++;continue;
    }
    closeList();
    html.push('<p>'+renderInline(line)+'</p>');
    i++;
  }
  closeList();closeTable();
  return html.join('\n');
}
fetch('/help.md').then(function(r){return r.text();}).then(function(md){
  document.getElementById('content').innerHTML = renderMarkdown(md);
  if (location.hash) {
    var el = document.getElementById(location.hash.slice(1));
    if (el) el.scrollIntoView();
  }
}).catch(function(e){
  document.getElementById('content').textContent = 'Failed to load help content: ' + e;
});
</script>
</body>
</html>
)HELPPAGE";
