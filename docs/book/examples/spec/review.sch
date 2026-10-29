<?xml version="1.0" encoding="UTF-8"?>
<!-- What a reviewer looks for in a Vane draft. -->
<schema xmlns="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3"
        schematronEdition="2025">
  <title>Review of a Vane draft</title>

  <let name="keywords" value="('MUST', 'MUST NOT', 'REQUIRED', 'SHALL',
    'SHALL NOT', 'SHOULD', 'SHOULD NOT', 'RECOMMENDED', 'NOT RECOMMENDED',
    'MAY', 'OPTIONAL')"/>

  <!-- Key words without their markup. In a pattern, only the first rule
       that matches a node fires for it. -->
  <pattern id="markup">
    <rule context="t[xref/@target = 'RFC2119']">
      <p>The boilerplate of BCP 14 quotes the key words on purpose.</p>
    </rule>
    <rule context="t"
          visit-each="tokenize(string-join(.//text()[not(ancestor::bcp14 | ancestor::cref)]),
                               '[^A-Z]+')[. = $keywords]">
      <report id="bare-keyword" test="true()"><value-of select="."/>
        without bcp14 markup.</report>
    </rule>
  </pattern>

  <!-- In a group, every rule that matches a node fires for it. -->
  <group id="paragraphs">
    <rule context="t[bcp14]">
      <let name="section" value="ancestor::section[1]"/>
      <assert id="anchored" test="@anchor" diagnostics="next-anchor">A
        requirement needs an anchor: the <value-of select="bcp14[1]"/> in
        "<value-of select="$section/name"/>" has none.</assert>
    </rule>
    <rule context="t[cref]">
      <report id="open-issue" severity="info" test="true()" subject="cref">Open
        issue <value-of select="@anchor"/> of <value-of select="@source"/>:
        <value-of select="normalize-space()"/></report>
    </rule>
  </group>

  <pattern id="references">
    <rule context="dt[@anchor] | reference[@anchor]">
      <assert id="used" severity="warning" test="@anchor = //xref/@target">The
        <name/> <value-of select="@anchor"/> is defined, and nothing refers
        to it.</assert>
    </rule>
  </pattern>

  <diagnostics>
    <diagnostic id="next-anchor">The next free anchor is req-<value-of
      select="max(//t/@anchor[starts-with(., 'req-')]
                  ! xs:integer(substring(., 5))) + 1"/>.</diagnostic>
  </diagnostics>
</schema>
