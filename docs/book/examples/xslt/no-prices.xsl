<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <!-- Copy every node that no template matches. -->
  <xsl:mode on-no-match="shallow-copy"/>

  <!-- Leave out the prices. -->
  <xsl:template match="price"/>

  <!-- Turn the tags into one element. -->
  <xsl:template match="tags">
    <keywords>{string-join(tag, ', ')}</keywords>
  </xsl:template>
</xsl:stylesheet>
