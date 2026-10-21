<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xmlns:spec="urn:example:spec"
    exclude-result-prefixes="spec">
  <xsl:import href="common.xsl"/>
  <xsl:use-package name="spec.xsl" package-version="1.0"/>
  <xsl:output method="json" indent="yes"/>

  <!-- The requirements as JSON, for tools: an array of objects. -->
  <xsl:template match="/">
    <xsl:sequence select="{
        'draft': string(rfc/@docName),
        'requirements': array { //t[@anchor] ! spec:requirement(.) }
      }"/>
  </xsl:template>

  <xsl:function name="spec:requirement" as="map(*)">
    <xsl:param name="t" as="element(t)"/>
    <xsl:sequence select="{
        'anchor': string($t/@anchor),
        'number': spec:number($t),
        'keyword': string($t/bcp14),
        'section': spec:number($t/ancestor::section[1]),
        'text': spec:text($t)
      }"/>
  </xsl:function>
</xsl:stylesheet>
