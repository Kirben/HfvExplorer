"""Regenerate standalone topics from the preserved manual (beautifulsoup4/html5lib)."""
from pathlib import Path
from html import escape
import re
from bs4 import BeautifulSoup, Comment, Tag

root=Path(__file__).resolve().parent
soup=BeautifulSoup((root/'HFVExplorerHelp.html').read_bytes(),'html5lib',from_encoding='iso-8859-1')
extra={'menu_file':'File menu','menu_edit':'Edit menu','menu_view':'View menu','menu_tools':'Tools menu','menu_help':'Help menu','command_options':'Options',
 'General_options':'General options','Copying_options':'Copying files options','File_attributes_options':'File attributes options','Shortcuts_options':'Shortcuts options',
 'Executor_DOS_options':'Executor DOS options','Executor_Win32_options':'Executor Win32 options','vMac_Win32_options':'vMac options','Shapeshifter_options':'Shapeshifter options','Fusion_DOS_options':'Fusion DOS options'}
topics=[]
for anchor in list(soup.find_all('a',attrs={'name':True})):
 name=anchor['name'];heading=anchor.find_parent(re.compile(r'^h[1-4]$'))
 if not heading and name not in extra:continue
 node=heading or anchor.find_parent(['p','blockquote']) or anchor
 # Some old menu anchors are inside an inherited blockquote; start at the
 # anchor itself rather than moving the marker before unrelated earlier text.
 if not heading and node.name=='blockquote':node=anchor
 title=extra.get(name) or heading.get_text(' ',strip=True)
 file='section-'+re.sub(r'[^a-z0-9-]','-',name.lower())+'.html'
 marker='HFV-TOPIC-'+str(len(topics))
 ancestors=list(reversed([p for p in node.parents if isinstance(p,Tag) and p.name not in ['html','body','[document]']]))
 opening=''.join('<'+p.name+''.join(' '+k+'="'+escape(' '.join(v) if isinstance(v,list) else str(v),quote=True)+'"' for k,v in p.attrs.items())+'>' for p in ancestors)
 node.insert_before(Comment(marker));topics.append((title,file,marker,opening))
body=soup.body.decode_contents();parts=[];anchors={}
for i,(title,file,marker,opening) in enumerate(topics):
 start=body.index('<!--'+marker+'-->')+len('<!--'+marker+'-->')
 end=body.index('<!--'+topics[i+1][2]+'-->') if i+1<len(topics) else len(body)
 fragment=opening+body[start:end]
 # Keep the introductory heading before the first named section.
 if i==0:fragment=body[:body.index('<!--'+marker+'-->')]+fragment
 page=BeautifulSoup('<!doctype html><html><head><meta charset="utf-8"><title>'+escape(title)+'</title></head><body>'+fragment+'</body></html>','html5lib')
 for a in page.find_all('a',attrs={'name':True}):anchors[a['name']]=file
 for a in page.find_all(attrs={'id':True}):anchors[a['id']]=file
 parts.append((title,file,page))
def remap(url):
 file,sep,fragment=url.partition('#')
 if file.lower()=='hfvexplorerhelp.html' or (not file and sep):
  if fragment in anchors:return anchors[fragment]+'#'+fragment
  if not fragment:return 'manual.html'
 return url
for title,file,page in parts:
 for tag in page.find_all(href=True):
  if tag['href']=='':tag.unwrap()
  else:tag['href']=remap(tag['href'])
 nav=page.new_tag('p');link=page.new_tag('a',href='manual.html');link.string='Manual contents';nav.append(link)
 page.body.insert(0,nav)
 (root/file).write_text(str(page),encoding='utf-8')
items=[('Current x64 build','start.html'),('Original manual contents','manual.html')]+[(t,f) for t,f,_ in parts]
index='<!doctype html><html><head><meta charset="utf-8"><title>Original manual contents</title></head><body><h1>Original manual</h1><p>Lauri Pesonen, January 1999. Each section opens as a separate page. See <a href="start.html">current x64 changes</a> before using historical instructions.</p><ul>'+''.join('<li><a href="'+f+'">'+escape(t)+'</a></li>' for t,f,_ in parts)+'</ul></body></html>'
(root/'manual.html').write_text(index,encoding='utf-8')
start=(root/'start.html').read_text(encoding='utf-8').replace('href="HFVExplorerHelp.html"','href="manual.html"')
(root/'start.html').write_text(start,encoding='utf-8')
def sitemap(items):return '<!DOCTYPE HTML PUBLIC "-//IETF//DTD HTML//EN"><HTML><BODY><UL>\n'+''.join('<LI><OBJECT type="text/sitemap"><param name="Name" value="'+escape(t,quote=True)+'"><param name="Local" value="'+escape(f,quote=True)+'"></OBJECT>\n' for t,f in items)+'</UL></BODY></HTML>'
for file,entries in [('HFVExplorer.hhc',items),('HFVExplorer.hhk',sorted(items))]:(root/file).write_text(sitemap(entries),encoding='cp1252',errors='xmlcharrefreplace')
project=root/'HFVExplorer.hhp';options=project.read_text(encoding='cp1252').split('[FILES]')[0]
files=['start.html','manual.html']+[f for _,f,_ in parts]+sorted(f.name for f in root.glob('*.gif'))
project.write_text(options+'[FILES]\n'+'\n'.join(files)+'\n',encoding='cp1252')
# Check every internal hyperlink/image/map after the rewrite.
for file in files:
 if not file.endswith('.html'):continue
 page=BeautifulSoup((root/file).read_text(encoding='utf-8'),'html5lib')
 for node in page.find_all(True):
  for attr in ['href','src','usemap']:
   url=node.get(attr)
   if not url or ':' in url:continue
   target,_,fragment=url.partition('#');target=target or file
   assert target in files,(file,url)
   if fragment:
    dest=BeautifulSoup((root/target).read_text(encoding='utf-8'),'html5lib')
    assert dest.find(attrs={'name':fragment}) or dest.find(id=fragment),(file,url)
print('PASS:',len(parts),'separate manual pages; all local links, anchors, screenshots and image maps resolve. Original single-page manual excluded from compiled help.')
