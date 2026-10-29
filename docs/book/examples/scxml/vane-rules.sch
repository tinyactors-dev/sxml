<?xml version="1.0" encoding="UTF-8"?>
<!-- Rules that the schemas of Vane's machines share. -->
<library xmlns="http://purl.oclc.org/dsdl/schematron">

  <!-- Each name in a list must be one of the known names.
       $context: the elements; $names: the attribute with the list;
       $known: the known names; $what: what a name names, as a string. -->
  <pattern abstract="true" id="names">
    <param name="context"/>
    <param name="names"/>
    <param name="known"/>
    <param name="what"/>
    <rule context="$context" visit-each="tokenize($names)">
      <let name="known-names" value="$known ! string()"/>
      <assert id="known-name" test=". = $known-names"><value-of select="."/>
        names no <value-of select="$what"/>.</assert>
    </rule>
  </pattern>

  <!-- A state or an event says in XHTML what it is for. -->
  <rules>
    <rule abstract="true" id="has-documentation">
      <assert id="documentation" severity="warning" test=".//h:p">The <name/>
        <value-of select="(@id, @name)[1]"/> has no documentation.</assert>
    </rule>
  </rules>
</library>
