<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xmlns:xs="http://www.w3.org/2001/XMLSchema"
    xmlns:spec="urn:example:spec"
    exclude-result-prefixes="xs spec">
  <!-- What the HTML, the text and the JSON output share. Each stylesheet
       that imports this module uses the package spec.xsl. -->

  <xsl:template match="xref">{spec:xref-text(.)}</xsl:template>

  <xsl:template match="cref"/>

  <!-- The text of a paragraph as a reader sees it. -->
  <xsl:function name="spec:text" as="xs:string">
    <xsl:param name="paragraph" as="element()"/>
    <xsl:variable name="text">
      <xsl:apply-templates select="$paragraph/node()"/>
    </xsl:variable>
    <xsl:sequence select="normalize-space($text)"/>
  </xsl:function>
</xsl:stylesheet>
